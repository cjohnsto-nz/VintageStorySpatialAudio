#pragma once

#include "audio/channel_layout.hpp"
#include "audio/vbap.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace vsa {

/// Decodes a world-space Ambisonic signal (Steam Audio's convention: ACN, orthonormal; up to
/// order 3) to the engine's speaker layouts, for the reflections (ADR 0009). All-round Ambisonic
/// decoding (AllRAD, Zotter & Frank): the sound field is sampled at a dense, even set of virtual
/// speakers with max-rE weighting, and each virtual speaker is panned onto the real ones with the
/// same VBAP the voices use (stereo: a constant-power left/right split). So 7.1.4 gets its
/// heights, and decoded sound sits where panned sound would.
///
/// Normalised so a plane wave decodes, on average over directions, with the power of a unit
/// panned voice (VBAP gains are power-normalised).
///
/// The constructor allocates; decode() never does.
class SpeakerDecoder {
public:
    /// `channels`: 2, 4, 6, 8 or 12 (the engine's layouts). `order`: 1..3.
    SpeakerDecoder(uint32_t channels, int order);

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t ambisonic_channels() const noexcept { return sh_channels_; }

    /// Adds the decoded `in` (ambisonic_channels() channels, world space) to `out` (channels()
    /// channels) for a listener with these axes (world space: right, up, ahead). The matrix
    /// follows the head from the previous block's across this one.
    void decode(const float right[3], const float up[3], const float ahead[3], const float* const* in,
                float* const* out, uint32_t frames) noexcept;

    /// Speaker gains for a plane wave from `direction` (listener space: +x right, +y up, -z
    /// ahead), as decode() would produce them: for tests.
    void plane_wave(const float direction[3], float* gains) const;

    /// Virtual speakers sampling the sphere.
    static constexpr int kVirtual = 64;

private:
    void matrix_for(const float right[3], const float up[3], const float ahead[3], float* m) const noexcept;

    uint32_t channels_;
    int order_;
    uint32_t sh_channels_;
    // Virtual speakers: listener-space directions and their real-speaker gains (channels_ each).
    std::array<std::array<float, 3>, kVirtual> virtual_{};
    std::vector<float> pan_;
    std::array<float, 16> weight_{};  // max-rE per channel, times the normalisation
    // Decode matrices, channels_ x sh_channels_: this block's and the previous one's.
    std::vector<float> matrix_;
    std::vector<float> previous_;
    bool primed_ = false;
};

}  // namespace vsa
