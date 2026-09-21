#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace vsa::dsp {

/// A second-order Butterworth high-pass (12 dB per octave below the corner; RBJ cookbook biquad,
/// Q = 1/sqrt 2): takes the weight out of a recording that has too much of it, footsteps chiefly.
/// Real-time safe.
class HighPass {
public:
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 2000.0f;
    /// Channels with their own state (a 7.1 bed's).
    static constexpr uint32_t kMaxChannels = 8;

    /// Recomputes coefficients (cheap, but not per sample). hz <= 0 bypasses the filter.
    void set(float hz, uint32_t sample_rate) noexcept {
        hz_ = hz;
        rate_ = sample_rate;
        active_ = hz > 0.0f && sample_rate > 0;
        if (!active_) {
            return;
        }
        const double f0 = std::clamp(static_cast<double>(hz), static_cast<double>(kMinHz), 0.45 * sample_rate);
        const double w0 = 2.0 * std::numbers::pi * f0 / sample_rate;
        const double cos_w = std::cos(w0);
        const double alpha = std::sin(w0) / std::numbers::sqrt2;  // sin(w0) / (2 Q), Q = 1/sqrt 2
        const double a0 = 1.0 + alpha;
        b0_ = static_cast<float>((1.0 + cos_w) / 2.0 / a0);
        b1_ = static_cast<float>(-(1.0 + cos_w) / a0);
        b2_ = b0_;
        a1_ = static_cast<float>(-2.0 * cos_w / a0);
        a2_ = static_cast<float>((1.0 - alpha) / a0);
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] float hz() const noexcept { return hz_; }
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
    float hz_ = 0.0f;
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
