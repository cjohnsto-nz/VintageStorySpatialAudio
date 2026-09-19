#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace vsa::dsp {

/// The late, diffuse part of a simulated reverb (ADR 0009): a 16-line feedback delay network
/// whose lines decay at the simulated RT60 per band (Steam Audio's three: below 800 Hz,
/// 800 Hz - 8 kHz, above 8 kHz), driven through a pre-delay and a 3-band level. It has
/// kOutputs mutually decorrelated outputs, meant to be encoded as plane waves from as many
/// directions: the tail then surrounds the listener instead of collapsing into one mono channel
/// (Steam Audio's own parametric reverb has a single output).
///
/// prepare() allocates; process() and reset() never do.
class LateReverb {
public:
    static constexpr int kLines = 16;
    static constexpr int kOutputs = 8;

    /// Delay lines for this rate; pre-delays up to `max_predelay` samples.
    void prepare(uint32_t sample_rate, uint32_t max_predelay);
    void reset() noexcept;

    /// Adds this block's kOutputs outputs (each `frames` long) to `out`.
    /// `rt60`: decay times per band, seconds. `level`: amplitude per band of the tail's start
    /// (Steam Audio's hybrid EQ). `predelay`: samples from the input to the tail's first echoes.
    /// Parameters are smoothed over about a second: each simulation run's estimate is noisy.
    /// All-zero parameters mean no result yet: nothing is injected, the input waits in the
    /// pre-delay. Returns false (writing nothing) when the input is
    /// silent and the tail has died away.
    bool process(const float* in, uint32_t frames, const float rt60[3], const float level[3], uint32_t predelay,
                 float* const* out) noexcept;

    /// The shortest delay line, in samples (pre-delays shorter than this start the tail later).
    [[nodiscard]] uint32_t min_delay() const noexcept { return lengths_[0]; }

private:
    /// First-order shelf: y = b0 x + b1 x1 - a1 y1.
    struct Shelf {
        float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;
        float x1 = 0.0f, y1 = 0.0f;
        float process(float x) noexcept {
            const float y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };
    struct Line {
        std::vector<float> buffer;  // power of two
        uint32_t mask = 0;
        uint32_t write = 0;
    };
    /// The lines' decay filters, one lane per line (laid out so the per-sample work vectorises):
    /// a low shelf, a high shelf and the mid band's gain per pass.
    struct Decay {
        alignas(32) float low_b0[kLines], low_b1[kLines], low_a1[kLines], low_x1[kLines], low_y1[kLines];
        alignas(32) float high_b0[kLines], high_b1[kLines], high_a1[kLines], high_x1[kLines], high_y1[kLines];
        alignas(32) float gain[kLines];
    };

    void update(const float rt60[3], const float level[3], uint32_t frames) noexcept;

    uint32_t rate_ = 48000;
    std::array<uint32_t, kLines> lengths_{};
    std::array<Line, kLines> lines_;
    Decay decay_{};
    float k_low_ = 0.0f;   // tan(pi fc / fs) at 800 Hz
    float k_high_ = 0.0f;  // and at 8 kHz

    // Input: pre-delay ring, then the 3-band level.
    std::vector<float> pre_;
    uint32_t pre_mask_ = 0;
    uint32_t pre_write_ = 0;
    Shelf in_low_, in_high_;
    float in_gain_ = 0.0f;

    float rt_[3] = {0.0f, 0.0f, 0.0f};
    float level_[3] = {0.0f, 0.0f, 0.0f};
    // The same, smoothed in the log domain (a level halving and doubling are equal steps).
    float log_rt_[3] = {0.0f, 0.0f, 0.0f};
    float log_level_[3] = {0.0f, 0.0f, 0.0f};
    bool primed_ = false;
    // Silence detection: blocks since the input was last non-silent, and the network's energy.
    uint32_t quiet_samples_ = 0;
    uint32_t drain_samples_ = 0;
    float energy_ = 0.0f;
};

}  // namespace vsa::dsp
