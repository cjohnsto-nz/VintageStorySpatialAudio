#pragma once

#include "vsaudio.h"

#include <cstdint>
#include <vector>

namespace vsa::dsp {

/// A read position in a source: whole frame plus a fraction in [0, 1).
struct SourcePosition {
    int64_t frame = 0;
    double fraction = 0.0;
};

/// Variable-rate resampler: bandlimited interpolation after J. O. Smith
/// (https://ccrma.stanford.edu/~jos/resample/).
///
/// The kernel is a Kaiser-windowed sinc sampled at kSamplesPerCrossing points per zero crossing
/// and linearly interpolated between them. When reading faster than 1 source frame per output
/// frame (ratio > 1) the kernel is stretched by the ratio, lowering its cutoff to the output
/// Nyquist, so the number of taps grows with the ratio (capped at kMaxRatio).
///
/// The kernel object is immutable after construction and shared by every voice; all per-voice
/// state is the SourcePosition, and the caller supplies the source frames as a window. process()
/// never allocates.
class ResamplerKernel {
public:
    static constexpr int kSamplesPerCrossing = 512;
    static constexpr double kMaxRatio = 8.0;

    explicit ResamplerKernel(vsa_resampler_quality quality);

    [[nodiscard]] int zero_crossings() const noexcept { return zero_crossings_; }
    [[nodiscard]] float rolloff() const noexcept { return rolloff_; }

    /// Taps on each side of the interpolation point at `ratio` source frames per output frame.
    [[nodiscard]] int taps_per_side(double ratio) const noexcept;
    [[nodiscard]] int max_taps_per_side() const noexcept { return taps_per_side(kMaxRatio); }

    /// Source frames [first, end) that process() reads to produce `frames` outputs from `pos`.
    struct Span {
        int64_t first;
        int64_t end;
    };
    [[nodiscard]] Span span(const SourcePosition& pos, double ratio, uint32_t frames) const noexcept;
    /// Upper bound of span().end - span().first for `frames` outputs at any supported ratio.
    [[nodiscard]] uint32_t max_span_frames(uint32_t frames) const noexcept;

    /// Renders `frames` output frames. window[c][i] holds source frame (window_first + i) of channel
    /// c and must cover span(pos, ratio, frames). Advances `pos`. `scratch` must hold
    /// scratch_floats() floats. `ratio` must be in (0, kMaxRatio].
    void process(const float* const* window, int64_t window_first, uint32_t channels, SourcePosition& pos,
                 double ratio, float* const* out, uint32_t frames, float* scratch) const noexcept;

    [[nodiscard]] std::size_t scratch_floats() const noexcept {
        return 2 * static_cast<std::size_t>(max_taps_per_side());
    }

    /// Advances `pos` by `frames` outputs at `ratio` exactly as process() would, without reading.
    static void advance(SourcePosition& pos, double ratio, uint32_t frames) noexcept;

private:
    int zero_crossings_;
    float rolloff_;
    // Linear layout (ratio > 1): h(i / L) and its forward difference, i in [0, (Nz + 1) * L],
    // zero beyond Nz crossings.
    std::vector<float> values_;
    std::vector<float> deltas_;
    // Polyphase layouts for ratio <= 1, so one interpolation point's coefficients are contiguous
    // and in the order the window is read (forward): the right wing's row p holds h(p / L + k)
    // for k in [0, Nz]; the left wing's row p holds h(p / L + Nz - 1 - m) for m in [0, Nz).
    std::vector<float> poly_values_;
    std::vector<float> poly_deltas_;
    std::vector<float> left_values_;
    std::vector<float> left_deltas_;
};

}  // namespace vsa::dsp
