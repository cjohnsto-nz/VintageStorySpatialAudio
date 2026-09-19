#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace vsa::dsp {

/// Stereo-linked, true-peak, look-ahead brickwall limiter.
///
///  1. Peak detection: |x| per sample plus the three 4x-oversampled points between samples
///     (8-tap polyphase windowed-sinc), so inter-sample peaks count.
///  2. Target gain g[n] = min(1, ceiling / peak[n]).
///  3. Minimum over a window of W samples (W = look-ahead), then exponential release (gain may
///     only rise slowly), then a W-sample moving average. The average of W values that are all
///     <= g[p] is <= g[p], so with the audio delayed by W - 1 samples the gain reaches the
///     target exactly when the peak passes, with a smooth attack ramp before it.
///
/// Latency: latency_frames(). prepare() allocates; process() never does.
class Limiter {
public:
    static constexpr float kDefaultCeilingDb = -1.0f;

    void prepare(uint32_t sample_rate, float lookahead_ms = 2.0f, float release_ms = 80.0f,
                 float ceiling_db = kDefaultCeilingDb);
    void reset() noexcept;

    /// In place on planar stereo. Returns the smallest gain applied in this call (1 = none).
    float process(float* left, float* right, uint32_t frames) noexcept;

    [[nodiscard]] uint32_t latency_frames() const noexcept { return delay_; }
    [[nodiscard]] float ceiling() const noexcept { return ceiling_; }

private:
    static constexpr int kTaps = 8;         // per oversampling phase
    static constexpr int kPhases = 3;       // interpolated points between two samples
    static constexpr int kDetectorLag = 4;  // the interpolator needs samples up to n + 4

    float ceiling_ = 0.891f;
    float release_coef_ = 0.0f;
    uint32_t window_ = 1;  // W
    uint32_t delay_ = 0;   // audio delay: detector lag + W - 1

    std::array<std::array<float, kTaps>, kPhases> fir_{};
    // Last kTaps input samples per channel, for the interpolator (index kTaps - 1 = newest).
    std::array<float, kTaps> hist_l_{};
    std::array<float, kTaps> hist_r_{};

    // Sliding-window minimum: monotonic deque in a ring of (value, sample index).
    std::vector<float> min_values_;
    std::vector<uint64_t> min_index_;
    uint32_t min_head_ = 0;
    uint32_t min_size_ = 0;

    float release_state_ = 1.0f;

    // Moving average.
    std::vector<float> box_;
    uint32_t box_pos_ = 0;
    double box_sum_ = 0.0;

    // Audio delay lines.
    std::vector<float> delay_l_;
    std::vector<float> delay_r_;
    uint32_t delay_pos_ = 0;

    uint64_t sample_index_ = 0;
};

}  // namespace vsa::dsp
