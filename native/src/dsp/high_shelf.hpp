#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace vsa::dsp {

/// The game's "low-pass" (OpenAL EFX AL_LOWPASS_GAINHF, used underwater), reproduced the way
/// OpenAL Soft implements it: a high shelf around 5 kHz whose top gain is `gain_hf`
/// (RBJ cookbook biquad, shelf slope 1). Real-time safe.
class HighShelf {
public:
    static constexpr double kReferenceHz = 5000.0;
    static constexpr float kMinGain = 0.001f;  // -60 dB, like OpenAL Soft
    /// Channels with their own state (a 7.1 bed's).
    static constexpr uint32_t kMaxChannels = 8;

    /// Recomputes coefficients (cheap, but not per sample). gain_hf >= 1 bypasses the filter.
    void set(float gain_hf, uint32_t sample_rate) noexcept {
        gain_ = gain_hf;
        rate_ = sample_rate;
        active_ = gain_hf < 0.999f;
        if (!active_) {
            return;
        }
        const auto g = static_cast<double>(std::max(gain_hf, kMinGain));
        const double a = std::sqrt(g);  // RBJ A = 10^(dB/40): the shelf reaches A^2 = g
        const double f0 = std::min(kReferenceHz, 0.45 * sample_rate);
        const double w0 = 2.0 * std::numbers::pi * f0 / sample_rate;
        const double cos_w = std::cos(w0);
        const double alpha = std::sin(w0) / 2.0 * std::numbers::sqrt2;  // slope S = 1
        const double sqrt_a2alpha = 2.0 * std::sqrt(a) * alpha;
        const double b0 = a * ((a + 1) + (a - 1) * cos_w + sqrt_a2alpha);
        const double b1 = -2 * a * ((a - 1) + (a + 1) * cos_w);
        const double b2 = a * ((a + 1) + (a - 1) * cos_w - sqrt_a2alpha);
        const double a0 = (a + 1) - (a - 1) * cos_w + sqrt_a2alpha;
        const double a1 = 2 * ((a - 1) - (a + 1) * cos_w);
        const double a2 = (a + 1) - (a - 1) * cos_w - sqrt_a2alpha;
        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] float gain() const noexcept { return gain_; }
    [[nodiscard]] uint32_t rate() const noexcept { return rate_; }

    void reset() noexcept {
        for (auto& z : state_) {
            z[0] = 0.0f;
            z[1] = 0.0f;
        }
    }

    /// In place, transposed direct form II. `channel` selects the state (< kMaxChannels).
    void process(float* x, uint32_t frames, uint32_t channel) noexcept {
        float z1 = state_[channel][0];
        float z2 = state_[channel][1];
        for (uint32_t j = 0; j < frames; ++j) {
            const float in = x[j];
            const float out = b0_ * in + z1;
            z1 = b1_ * in - a1_ * out + z2;
            z2 = b2_ * in - a2_ * out;
            x[j] = out;
        }
        // Flush decaying state before it turns denormal (slow on x86) after the input goes silent.
        state_[channel][0] = std::abs(z1) < 1e-20f ? 0.0f : z1;
        state_[channel][1] = std::abs(z2) < 1e-20f ? 0.0f : z2;
    }

private:
    float gain_ = 1.0f;
    uint32_t rate_ = 0;
    bool active_ = false;
    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;
    float state_[kMaxChannels][2] = {};
};

}  // namespace vsa::dsp
