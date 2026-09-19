#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vsa::dsp {

/// A gain that moves to a target either linearly (parameter smoothing) or geometrically
/// (linear in decibels, for fades). Real-time safe: no allocation, no locks.
class GainRamp {
public:
    /// Geometric ramps treat 0 as this gain (-80 dB) and snap to the exact target at the end.
    static constexpr double kGeometricFloor = 1e-4;

    GainRamp() noexcept : GainRamp(1.0f) {}
    explicit GainRamp(float value) noexcept : value_(static_cast<double>(value)), target_(value_) {}

    void jump(float value) noexcept {
        value_ = static_cast<double>(value);
        target_ = value_;
        remaining_ = 0;
    }

    void linear(float target, uint32_t frames) noexcept {
        target_ = static_cast<double>(target);
        if (frames == 0) {
            jump(target);
            return;
        }
        geometric_ = false;
        step_ = (target_ - value_) / frames;
        remaining_ = frames;
    }

    void geometric(float target, uint32_t frames) noexcept {
        target_ = static_cast<double>(target);
        if (frames == 0) {
            jump(target);
            return;
        }
        geometric_ = true;
        const double from = std::max(value_, kGeometricFloor);
        const double to = std::max(target_, kGeometricFloor);
        value_ = from;
        step_ = std::pow(to / from, 1.0 / frames);
        remaining_ = frames;
    }

    [[nodiscard]] bool active() const noexcept { return remaining_ > 0; }
    [[nodiscard]] float value() const noexcept { return static_cast<float>(value_); }
    [[nodiscard]] float target() const noexcept { return static_cast<float>(target_); }

    /// Produces `frames` per-frame gains into `out` (nullptr to only advance). Returns true if the
    /// ramp reached its target during this call.
    bool render(float* out, uint32_t frames) noexcept {
        if (remaining_ == 0) {
            if (out != nullptr) {
                std::fill(out, out + frames, static_cast<float>(value_));
            }
            return false;
        }
        const uint32_t n = std::min(frames, remaining_);
        if (out == nullptr) {
            value_ = geometric_ ? value_ * std::pow(step_, static_cast<double>(n)) : value_ + step_ * n;
        } else if (geometric_) {
            for (uint32_t j = 0; j < n; ++j) {
                value_ *= step_;
                out[j] = static_cast<float>(value_);
            }
        } else {
            for (uint32_t j = 0; j < n; ++j) {
                value_ += step_;
                out[j] = static_cast<float>(value_);
            }
        }
        remaining_ -= n;
        if (remaining_ > 0) {
            return false;
        }
        value_ = target_;
        if (out != nullptr) {
            out[n - 1] = static_cast<float>(value_);
            std::fill(out + n, out + frames, static_cast<float>(value_));
        }
        return true;
    }

private:
    double value_;
    double target_;
    double step_ = 0.0;
    uint32_t remaining_ = 0;
    bool geometric_ = false;
};

}  // namespace vsa::dsp
