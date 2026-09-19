#include "audio/speaker_decoder.hpp"

#include "dsp/spherical_harmonics.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa {
namespace {

/// Points spread evenly over the sphere (a Fibonacci lattice).
void fibonacci(int count, int index, float* direction) {
    const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
    const double y = 1.0 - (2.0 * index + 1.0) / count;
    const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
    const double phi = golden * index;
    direction[0] = static_cast<float>(r * std::cos(phi));
    direction[1] = static_cast<float>(y);
    direction[2] = static_cast<float>(r * std::sin(phi));
}

/// max-rE weights per order (Zotter & Frank's approximation: P_n(cos(137.9 deg / (N + 1.51)))).
double max_re(int order, int n) {
    const double x = std::cos(137.9 / (order + 1.51) * std::numbers::pi / 180.0);
    switch (n) {
        case 0: return 1.0;
        case 1: return x;
        case 2: return 0.5 * (3.0 * x * x - 1.0);
        default: return 0.5 * (5.0 * x * x * x - 3.0 * x);
    }
}

constexpr float kIdentityRight[3] = {1.0f, 0.0f, 0.0f};
constexpr float kIdentityUp[3] = {0.0f, 1.0f, 0.0f};
constexpr float kIdentityAhead[3] = {0.0f, 0.0f, -1.0f};

}  // namespace

SpeakerDecoder::SpeakerDecoder(uint32_t channels, int order)
    : channels_(std::min(channels, kMaxOutputChannels)),
      order_(std::clamp(order, 1, 3)),
      sh_channels_(static_cast<uint32_t>((order_ + 1) * (order_ + 1))) {
    pan_.assign(static_cast<std::size_t>(kVirtual) * channels_, 0.0f);
    std::unique_ptr<Vbap> vbap;
    if (channels_ > 2) {
        vbap = std::make_unique<Vbap>(channels_);
    }
    for (int i = 0; i < kVirtual; ++i) {
        float* d = virtual_[static_cast<std::size_t>(i)].data();
        fibonacci(kVirtual, i, d);
        float* gains = pan_.data() + static_cast<std::size_t>(i) * channels_;
        if (vbap) {
            vbap->gains(d, gains);
        } else {
            // Stereo: a constant-power split by how far right the direction is.
            gains[0] = std::sqrt(0.5f * (1.0f - d[0]));
            gains[1] = std::sqrt(0.5f * (1.0f + d[0]));
        }
    }
    for (uint32_t c = 0; c < sh_channels_; ++c) {
        const int n = static_cast<int>(std::sqrt(static_cast<double>(c)));
        weight_[c] = static_cast<float>(max_re(order_, n) * 4.0 * std::numbers::pi / kVirtual);
    }

    // Normalise: plane waves from an even spread of directions decode at unit power on average.
    matrix_.assign(static_cast<std::size_t>(channels_) * sh_channels_, 0.0f);
    previous_ = matrix_;
    constexpr int kProbes = 1000;
    double power = 0.0;
    std::vector<float> gains(channels_);
    for (int p = 0; p < kProbes; ++p) {
        float d[3];
        fibonacci(kProbes, p, d);
        plane_wave(d, gains.data());
        for (const float g : gains) {
            power += static_cast<double>(g) * static_cast<double>(g);
        }
    }
    const auto norm = static_cast<float>(1.0 / std::sqrt(power / kProbes));
    for (uint32_t c = 0; c < sh_channels_; ++c) {
        weight_[c] *= norm;
    }
}

void SpeakerDecoder::matrix_for(const float right[3], const float up[3], const float ahead[3], float* m) const noexcept {
    std::fill(m, m + static_cast<std::size_t>(channels_) * sh_channels_, 0.0f);
    float sh[dsp::kSh3Channels];
    for (int i = 0; i < kVirtual; ++i) {
        const float* v = virtual_[static_cast<std::size_t>(i)].data();
        // The virtual speaker's direction in world space (listener space is +x right, +y up, -z ahead).
        float world[3];
        for (int k = 0; k < 3; ++k) {
            world[k] = right[k] * v[0] + up[k] * v[1] - ahead[k] * v[2];
        }
        dsp::sh_order3(world, sh);
        const float* gains = pan_.data() + static_cast<std::size_t>(i) * channels_;
        for (uint32_t s = 0; s < channels_; ++s) {
            const float g = gains[s];
            if (g == 0.0f) {
                continue;
            }
            float* row = m + static_cast<std::size_t>(s) * sh_channels_;
            for (uint32_t c = 0; c < sh_channels_; ++c) {
                row[c] += g * weight_[c] * sh[c];
            }
        }
    }
}

void SpeakerDecoder::plane_wave(const float direction[3], float* gains) const {
    std::vector<float> m(static_cast<std::size_t>(channels_) * sh_channels_);
    matrix_for(kIdentityRight, kIdentityUp, kIdentityAhead, m.data());
    float sh[dsp::kSh3Channels];
    dsp::sh_order3(direction, sh);
    for (uint32_t s = 0; s < channels_; ++s) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < sh_channels_; ++c) {
            sum += m[static_cast<std::size_t>(s) * sh_channels_ + c] * sh[c];
        }
        gains[s] = sum;
    }
}

void SpeakerDecoder::decode(const float right[3], const float up[3], const float ahead[3], const float* const* in,
                            float* const* out, uint32_t frames) noexcept {
    previous_.swap(matrix_);
    matrix_for(right, up, ahead, matrix_.data());
    if (!primed_) {
        previous_ = matrix_;  // same size: no allocation
        primed_ = true;
    }
    const float step = 1.0f / static_cast<float>(frames);
    for (uint32_t s = 0; s < channels_; ++s) {
        float* y = out[s];
        for (uint32_t c = 0; c < sh_channels_; ++c) {
            const std::size_t at = static_cast<std::size_t>(s) * sh_channels_ + c;
            const float from = previous_[at];
            const float to = matrix_[at];
            if (from == 0.0f && to == 0.0f) {
                continue;  // the LFE
            }
            const float change = (to - from) * step;
            const float* x = in[c];
            for (uint32_t j = 0; j < frames; ++j) {
                y[j] += (from + change * static_cast<float>(j + 1)) * x[j];
            }
        }
    }
}

}  // namespace vsa
