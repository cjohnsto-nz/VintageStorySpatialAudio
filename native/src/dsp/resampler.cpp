#include "dsp/resampler.hpp"

#include "dsp/simd.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace vsa::dsp {
namespace {

struct QualityParams {
    int zero_crossings;
    double kaiser_beta;
    // Cutoff as a fraction of the (lower) Nyquist frequency. Below 1 so the window's transition
    // band sits mostly under Nyquist, trading the top of the audio band for less aliasing.
    double rolloff;
};

QualityParams params_for(vsa_resampler_quality quality) noexcept {
    switch (quality) {
        case VSA_RESAMPLER_LOW: return {4, 5.0, 0.85};
        case VSA_RESAMPLER_HIGH: return {16, 9.0, 0.94};
        case VSA_RESAMPLER_DEFAULT:
        case VSA_RESAMPLER_MEDIUM: break;
    }
    return {8, 7.0, 0.90};
}

// Zeroth-order modified Bessel function of the first kind (power series).
double bessel_i0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double quarter_x2 = x * x / 4.0;
    for (int k = 1; k < 200; ++k) {
        term *= quarter_x2 / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

}  // namespace

ResamplerKernel::ResamplerKernel(vsa_resampler_quality quality) {
    const QualityParams params = params_for(quality);
    zero_crossings_ = params.zero_crossings;
    rolloff_ = static_cast<float>(params.rolloff);

    constexpr int L = kSamplesPerCrossing;
    const auto n = static_cast<std::size_t>(zero_crossings_) * L;
    std::vector<double> h(n + 1, 0.0);
    const double i0_beta = bessel_i0(params.kaiser_beta);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / L;
        const double x = t / zero_crossings_;
        const double window = bessel_i0(params.kaiser_beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0_beta;
        const double arg = std::numbers::pi * params.rolloff * t;
        const double sinc = i == 0 ? 1.0 : std::sin(arg) / arg;
        h[i] = params.rolloff * sinc * window;
    }
    h[n] = 0.0;

    // Unity gain at DC: the taps at integer offsets sum to 1.
    double dc = h[0];
    for (int k = 1; k < zero_crossings_; ++k) {
        dc += 2.0 * h[static_cast<std::size_t>(k) * L];
    }
    for (double& value : h) {
        value /= dc;
    }

    const auto h_at = [&](std::size_t i) { return i <= n ? h[i] : 0.0; };

    // Zero padding past Nz crossings: the stretched path's last tap can reach up to one crossing
    // beyond (see process()), and padding avoids a bounds branch per tap.
    const std::size_t linear_size = n + L + 2;
    values_.resize(linear_size);
    deltas_.resize(linear_size);
    for (std::size_t i = 0; i < linear_size; ++i) {
        values_[i] = static_cast<float>(h_at(i));
        deltas_[i] = static_cast<float>(h_at(i + 1) - h_at(i));
    }

    const auto nz = static_cast<std::size_t>(zero_crossings_);
    const std::size_t row = nz + 1;
    poly_values_.resize(L * row);
    poly_deltas_.resize(L * row);
    left_values_.resize(L * nz);
    left_deltas_.resize(L * nz);
    for (std::size_t phase = 0; phase < L; ++phase) {
        for (std::size_t k = 0; k < row; ++k) {
            const std::size_t i = phase + k * L;
            poly_values_[phase * row + k] = static_cast<float>(h_at(i));
            poly_deltas_[phase * row + k] = static_cast<float>(h_at(i + 1) - h_at(i));
        }
        for (std::size_t m = 0; m < nz; ++m) {
            const std::size_t i = phase + (nz - 1 - m) * L;
            left_values_[phase * nz + m] = static_cast<float>(h_at(i));
            left_deltas_[phase * nz + m] = static_cast<float>(h_at(i + 1) - h_at(i));
        }
    }
}

int ResamplerKernel::taps_per_side(double ratio) const noexcept {
    if (ratio <= 1.0) {
        return zero_crossings_;
    }
    return static_cast<int>(std::ceil(zero_crossings_ * std::min(ratio, kMaxRatio)));
}

ResamplerKernel::Span ResamplerKernel::span(const SourcePosition& pos, double ratio, uint32_t frames) const noexcept {
    const int taps = taps_per_side(ratio);
    const int64_t first = pos.frame - taps + 1;
    if (frames == 0) {
        return {first, first};
    }
    const auto last = pos.frame + static_cast<int64_t>(std::floor(pos.fraction + (frames - 1) * ratio));
    // +1 beyond the last tap covers floating-point drift between this closed form and the
    // incremental stepping in process().
    return {first, last + taps + 2};
}

uint32_t ResamplerKernel::max_span_frames(uint32_t frames) const noexcept {
    const auto stepped = static_cast<uint32_t>(std::ceil(static_cast<double>(frames) * kMaxRatio));
    return stepped + 2 * static_cast<uint32_t>(max_taps_per_side()) + 4;
}

void ResamplerKernel::advance(SourcePosition& pos, double ratio, uint32_t frames) noexcept {
    int64_t frame = pos.frame;
    double fraction = pos.fraction;
    for (uint32_t j = 0; j < frames; ++j) {
        fraction += ratio;
        const auto whole = static_cast<int64_t>(fraction);  // fraction >= 0: truncation == floor
        frame += whole;
        fraction -= static_cast<double>(whole);
    }
    pos.frame = frame;
    pos.fraction = fraction;
}


void ResamplerKernel::process(const float* const* window, int64_t window_first, uint32_t channels,
                              SourcePosition& pos, double ratio, float* const* out, uint32_t frames,
                              float* scratch) const noexcept {
    int64_t frame = pos.frame;
    double fraction = pos.fraction;

    // Unit rate on a whole frame: a plain copy, bit exact.
    if (ratio == 1.0 && fraction == 0.0) {
        for (uint32_t c = 0; c < channels; ++c) {
            const float* x = window[c] + (frame - window_first);
            std::copy(x, x + frames, out[c]);
        }
        pos.frame = frame + frames;
        return;
    }

    constexpr int L = kSamplesPerCrossing;
    const int taps = taps_per_side(ratio);
    const auto width = static_cast<std::size_t>(2 * taps);
    // coef[m] weights source frame (frame - taps + 1 + m): m < taps is the left wing (reversed),
    // m >= taps the right wing. One contiguous dot product per channel.
    float* coef = scratch;

    const bool stretched = ratio > 1.0;
    const double scale = stretched ? 1.0 / ratio : 1.0;
    const auto gain = static_cast<float>(scale);
    const auto nz = static_cast<std::size_t>(zero_crossings_);
    const std::size_t row = nz + 1;

    for (uint32_t j = 0; j < frames; ++j) {
        if (!stretched) {
            // Left wing: frame - k sits (fraction + k) crossings away. The fine phase is the same
            // for every k, so the coefficients are one contiguous (pre-reversed) polyphase row.
            const double pl = fraction * L;
            const auto il = static_cast<std::size_t>(static_cast<int64_t>(pl));
            const auto el = static_cast<float>(pl - static_cast<double>(il));
            const float* vl = &left_values_[il * nz];
            const float* dl = &left_deltas_[il * nz];
            // taps == Nz here, a multiple of 4 for every quality.
            const simd::f4 elv = simd::splat(el);
            for (int m = 0; m < taps; m += 4) {
                simd::store(coef + m, simd::madd(elv, simd::load(dl + m), simd::load(vl + m)));
            }
            // Right wing: frame + 1 + k sits (1 - fraction + k) crossings away.
            const double pr = (1.0 - fraction) * L;
            auto ir = static_cast<std::size_t>(static_cast<int64_t>(pr));
            const auto er = static_cast<float>(pr - static_cast<double>(ir));
            std::size_t shift = 0;
            if (ir >= static_cast<std::size_t>(L)) {  // fraction == 0: distances are exactly 1 + k
                ir -= L;
                shift = 1;
            }
            const float* vr = &poly_values_[ir * row + shift];
            const float* dr = &poly_deltas_[ir * row + shift];
            const simd::f4 erv = simd::splat(er);
            for (int k = 0; k < taps; k += 4) {
                simd::store(coef + taps + k, simd::madd(erv, simd::load(dr + k), simd::load(vr + k)));
            }
        } else {
            // Stretched kernel: tap k sits scale * (fraction + k) crossings away (left) or
            // scale * (1 - fraction + k) (right). taps = ceil(Nz / scale), so the farthest tap is
            // below Nz + 1 crossings, inside the zero-padded table.
            const double step = scale * L;
            const double left0 = scale * fraction * L;
            const double right0 = scale * (1.0 - fraction) * L;
            for (int k = 0; k < taps; ++k) {
                const double pl = left0 + k * step;
                const auto il = static_cast<std::size_t>(static_cast<int64_t>(pl));
                coef[taps - 1 - k] = values_[il] + static_cast<float>(pl - static_cast<double>(il)) * deltas_[il];
                const double pr = right0 + k * step;
                const auto ir = static_cast<std::size_t>(static_cast<int64_t>(pr));
                coef[taps + k] = values_[ir] + static_cast<float>(pr - static_cast<double>(ir)) * deltas_[ir];
            }
        }

        // The dot product dominates the render cost: 4-wide, with a scalar tail for stretched
        // kernels whose width is not a multiple of 4.
        const int64_t base = frame - taps + 1 - window_first;
        const std::size_t vector_width = width & ~std::size_t{3};
        for (uint32_t c = 0; c < channels; ++c) {
            const float* x = window[c] + base;
            simd::f4 acc = simd::zero();
            for (std::size_t m = 0; m < vector_width; m += 4) {
                acc = simd::madd(simd::load(x + m), simd::load(coef + m), acc);
            }
            float sum = simd::hsum(acc);
            for (std::size_t m = vector_width; m < width; ++m) {
                sum += x[m] * coef[m];
            }
            out[c][j] = sum * gain;
        }

        // Must match advance() exactly.
        fraction += ratio;
        const auto whole = static_cast<int64_t>(fraction);
        frame += whole;
        fraction -= static_cast<double>(whole);
    }

    pos.frame = frame;
    pos.fraction = fraction;
}

}  // namespace vsa::dsp
