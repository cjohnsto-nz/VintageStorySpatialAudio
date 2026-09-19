#include "dsp/late_reverb.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa::dsp {
namespace {

// Mutually prime line lengths at 48 kHz, 21-61 ms: dense enough for a tail that starts where the
// convolved early reflections end (ADR 0009).
constexpr std::array<uint32_t, LateReverb::kLines> kLengths48k = {1009, 1123, 1237, 1361, 1489, 1613, 1733, 1861,
                                                                   1987, 2111, 2239, 2371, 2503, 2633, 2767, 2903};

// Steam Audio's band edges.
constexpr double kLowEdge = 800.0;
constexpr double kHighEdge = 8000.0;

// The tail's level from Steam Audio's hybrid estimate (its "EQ": the level where the convolved part
// ends): the injected input is scaled so the tail carries on from Steam Audio's own simulated
// impulse response, which the estimate undershoots by more the shorter the decay. Fitted against
// that response in rooms decaying in 0.4-2.5 s (core/test_reflection_calibration.cpp): level x
// kInputCalibration x RT60^kDecayExponent. (Steam Audio's own hybrid tail sits some 6 dB below
// the response.)
constexpr float kInputCalibration = 0.635f;
constexpr float kDecayExponent = -0.385f;

// Each output is a pair of lines, at unit power.
constexpr float kPairScale = 0.70710678f;

// The simulation's level and decay estimates are noisy from one run to the next (a level is
// read from one 10 ms slice of a ray-traced energy field): they are followed this slowly.
constexpr float kLevelSeconds = 0.7f;
constexpr float kDecaySeconds = 1.0f;
constexpr float kLevelFloor = 1e-6f;

// Tails quieter than this (sum of squares over a block) are over.
constexpr float kSilentEnergy = 1e-14f;

uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/// First-order shelves from complementary first-order low and high passes around `k` =
/// tan(pi fc / fs): low shelf G*LP + HP (gain G at DC, 1 at Nyquist), high shelf LP + G*HP.
void low_shelf(float k, float g, float& b0, float& b1, float& a1) {
    const float n = 1.0f / (1.0f + k);
    b0 = (g * k + 1.0f) * n;
    b1 = (g * k - 1.0f) * n;
    a1 = (k - 1.0f) * n;
}

void high_shelf(float k, float g, float& b0, float& b1, float& a1) {
    const float n = 1.0f / (1.0f + k);
    b0 = (k + g) * n;
    b1 = (k - g) * n;
    a1 = (k - 1.0f) * n;
}

/// In-place fast Walsh-Hadamard transform of 16 values (unnormalised).
inline void fwht16(float* v) noexcept {
    for (int h = 1; h < LateReverb::kLines; h <<= 1) {
        for (int i = 0; i < LateReverb::kLines; i += h << 1) {
            for (int j = i; j < i + h; ++j) {
                const float a = v[j];
                const float b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
        }
    }
}

}  // namespace

void LateReverb::prepare(uint32_t sample_rate, uint32_t max_predelay) {
    rate_ = sample_rate;
    const double scale = static_cast<double>(sample_rate) / 48000.0;
    for (int i = 0; i < kLines; ++i) {
        lengths_[static_cast<std::size_t>(i)] =
            std::max(64u, static_cast<uint32_t>(std::lround(kLengths48k[static_cast<std::size_t>(i)] * scale)) | 1u);
    }
    for (int i = 0; i < kLines; ++i) {
        Line& line = lines_[static_cast<std::size_t>(i)];
        const uint32_t size = next_pow2(lengths_[static_cast<std::size_t>(i)] + 1);
        line.buffer.assign(size, 0.0f);
        line.mask = size - 1;
    }
    const auto rate = static_cast<double>(sample_rate);
    k_low_ = static_cast<float>(std::tan(std::numbers::pi * kLowEdge / rate));
    k_high_ = static_cast<float>(std::tan(std::numbers::pi * std::min(kHighEdge, 0.45 * rate) / rate));
    // Room for the pre-delay plus one chunk (at most the shortest line).
    const uint32_t pre_size = next_pow2(max_predelay + lengths_[0] + 1);
    pre_.assign(pre_size, 0.0f);
    pre_mask_ = pre_size - 1;
    reset();
}

void LateReverb::reset() noexcept {
    for (Line& line : lines_) {
        std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
        line.write = 0;
    }
    std::fill(std::begin(decay_.low_x1), std::end(decay_.low_x1), 0.0f);
    std::fill(std::begin(decay_.low_y1), std::end(decay_.low_y1), 0.0f);
    std::fill(std::begin(decay_.high_x1), std::end(decay_.high_x1), 0.0f);
    std::fill(std::begin(decay_.high_y1), std::end(decay_.high_y1), 0.0f);
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    pre_write_ = 0;
    in_low_.x1 = in_low_.y1 = 0.0f;
    in_high_.x1 = in_high_.y1 = 0.0f;
    primed_ = false;
    quiet_samples_ = 0;
    drain_samples_ = 0;
    energy_ = 0.0f;
}

void LateReverb::update(const float rt60[3], const float level[3], uint32_t frames) noexcept {
    const auto seconds = static_cast<float>(frames) / static_cast<float>(rate_);
    const float level_step = 1.0f - std::exp(-seconds / kLevelSeconds);
    const float decay_step = 1.0f - std::exp(-seconds / kDecaySeconds);
    for (int b = 0; b < 3; ++b) {
        const float rt = std::log(std::clamp(std::isfinite(rt60[b]) ? rt60[b] : 0.0f, 0.05f, 20.0f));
        const float lv = std::log(std::clamp(std::isfinite(level[b]) ? level[b] : 0.0f, 0.0f, 16.0f) + kLevelFloor);
        if (!primed_) {
            log_rt_[b] = rt;
            log_level_[b] = lv;
        } else {
            log_rt_[b] += decay_step * (rt - log_rt_[b]);
            log_level_[b] += level_step * (lv - log_level_[b]);
        }
        rt_[b] = std::exp(log_rt_[b]);
        level_[b] = std::max(0.0f, std::exp(log_level_[b]) - kLevelFloor);
    }
    primed_ = true;

    const auto rate = static_cast<float>(rate_);
    for (int i = 0; i < kLines; ++i) {
        const auto length = static_cast<float>(lengths_[static_cast<std::size_t>(i)]);
        float g[3];
        for (int b = 0; b < 3; ++b) {
            g[b] = std::pow(10.0f, -3.0f * length / (rt_[b] * rate));  // -60 dB after rt seconds
        }
        const float low_ratio = g[0] / g[1];
        const float high_ratio = g[2] / g[1];
        // Keep the loop gain below 1 at every frequency (the shelves overlap a little).
        float mid = g[1];
        const float bound = mid * std::max(1.0f, low_ratio) * std::max(1.0f, high_ratio);
        if (bound > 0.9999f) {
            mid *= 0.9999f / bound;
        }
        decay_.gain[i] = mid;
        low_shelf(k_low_, low_ratio, decay_.low_b0[i], decay_.low_b1[i], decay_.low_a1[i]);
        high_shelf(k_high_, high_ratio, decay_.high_b0[i], decay_.high_b1[i], decay_.high_a1[i]);
    }

    float calibrated[3];
    for (int b = 0; b < 3; ++b) {
        calibrated[b] = level_[b] * std::pow(rt_[b], kDecayExponent);
    }
    const float reference = std::max({calibrated[0], calibrated[1], calibrated[2], 1e-9f});
    const float mid = std::max(calibrated[1], 1e-4f * reference);
    low_shelf(k_low_, calibrated[0] / mid, in_low_.b0, in_low_.b1, in_low_.a1);
    high_shelf(k_high_, calibrated[2] / mid, in_high_.b0, in_high_.b1, in_high_.a1);
    // Mean line length: the network releases its energy over one pass.
    float mean = 0.0f;
    for (const uint32_t length : lengths_) {
        mean += static_cast<float>(length);
    }
    mean /= static_cast<float>(kLines);
    in_gain_ = mid * kInputCalibration * std::sqrt(mean / static_cast<float>(kLines));
}

bool LateReverb::process(const float* in, uint32_t frames, const float rt60[3], const float level[3],
                         uint32_t predelay, float* const* out) noexcept {
    bool input = false;
    for (uint32_t j = 0; j < frames && !input; ++j) {
        input = in[j] != 0.0f;
    }
    if (input) {
        quiet_samples_ = 0;
        drain_samples_ = 0;
    } else {
        quiet_samples_ = quiet_samples_ + frames;
        if (energy_ < kSilentEnergy && quiet_samples_ > predelay + lengths_[kLines - 1]) {
            if (drain_samples_ == 0) {
                reset();  // clean state (no denormals) for the next input
                drain_samples_ = 1;
            }
            return false;
        }
    }

    update(rt60, level, frames);
    // The network's first echoes (after its shortest line) arrive when the tail starts.
    predelay = predelay > lengths_[0] ? std::min(predelay - lengths_[0], pre_mask_ - lengths_[0]) : 0u;

    // In chunks no longer than the shortest line: every line's output for the chunk is then
    // already in its buffer.
    alignas(32) float y[kLines];
    float energy = 0.0f;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(frames - done, lengths_[0]);
        for (uint32_t j = 0; j < n; ++j) {
            pre_[(pre_write_ + j) & pre_mask_] = in[done + j];
        }
        for (uint32_t j = 0; j < n; ++j) {
            const float delayed = pre_[(pre_write_ + j - predelay) & pre_mask_];
            const float x = in_gain_ * in_high_.process(in_low_.process(delayed));
            for (int i = 0; i < kLines; ++i) {
                const Line& line = lines_[static_cast<std::size_t>(i)];
                y[i] = line.buffer[(line.write + j - lengths_[static_cast<std::size_t>(i)]) & line.mask];
            }
            Decay& d = decay_;
            for (int i = 0; i < kLines; ++i) {
                const float low = d.low_b0[i] * y[i] + d.low_b1[i] * d.low_x1[i] - d.low_a1[i] * d.low_y1[i];
                d.low_x1[i] = y[i];
                d.low_y1[i] = low;
                const float high = d.high_b0[i] * low + d.high_b1[i] * d.high_x1[i] - d.high_a1[i] * d.high_y1[i];
                d.high_x1[i] = low;
                d.high_y1[i] = high;
                y[i] = d.gain[i] * high;
            }
            // Outputs: each from its own pair of lines, so they are independent and every line
            // reaches the pressure (their sum). Feedback: all sixteen, mixed (Hadamard).
            for (int k = 0; k < kOutputs; ++k) {
                const float a = y[2 * k];
                const float b = y[2 * k + 1];
                out[k][done + j] += kPairScale * (a - b);
                energy += a * a + b * b;
            }
            fwht16(y);
            for (int i = 0; i < kLines; ++i) {
                Line& line = lines_[static_cast<std::size_t>(i)];
                line.buffer[(line.write + j) & line.mask] = 0.25f * y[i] + ((i & 1) != 0 ? -x : x);
            }
        }
        for (Line& line : lines_) {
            line.write = (line.write + n) & line.mask;
        }
        pre_write_ = (pre_write_ + n) & pre_mask_;
        done += n;
    }
    energy_ = energy;
    return true;
}

}  // namespace vsa::dsp
