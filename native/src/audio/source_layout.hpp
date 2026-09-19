#pragma once

#include "decode/decoders.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace vsa {

/// Where one channel of a multichannel asset is meant to play: a speaker direction (azimuth in
/// degrees clockwise from ahead, elevation up), or the LFE, which has none.
struct SourceChannel {
    float azimuth = 0.0f;
    float elevation = 0.0f;
    bool lfe = false;

    /// Unit direction in listener space (+x right, +y up, -z ahead), as Vbap and the Ambisonic
    /// encoder take it.
    void direction(float out[3]) const noexcept {
        constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;
        const float az = azimuth * kRadians;
        const float el = elevation * kRadians;
        out[0] = std::sin(az) * std::cos(el);
        out[1] = std::sin(el);
        out[2] = -std::cos(az) * std::cos(el);
    }
};

using SourceLayout = std::array<SourceChannel, decode::kMaxChannels>;

/// The surrounds of a layout with one surround pair (quad, 5.0, 5.1): ITU-R BS.775's 110 degrees.
inline constexpr float kSurroundAzimuth = 110.0f;

/// Ogg Vorbis: the channel order its specification fixes for each count (section 4.3.9).
[[nodiscard]] inline SourceLayout vorbis_layout(uint32_t channels) noexcept {
    constexpr SourceChannel fl{-30.0f}, fr{30.0f}, c{0.0f}, lfe{0.0f, 0.0f, true};
    constexpr SourceChannel sl{-kSurroundAzimuth}, sr{kSurroundAzimuth};  // the only surround pair
    constexpr SourceChannel sl7{-90.0f}, sr7{90.0f}, bl7{-150.0f}, br7{150.0f}, bc{180.0f};
    SourceLayout layout{};
    switch (channels) {
        case 1: layout = {c}; break;
        case 2: layout = {fl, fr}; break;
        case 3: layout = {fl, c, fr}; break;
        case 4: layout = {fl, fr, sl, sr}; break;
        case 5: layout = {fl, c, fr, sl, sr}; break;
        case 6: layout = {fl, c, fr, sl, sr, lfe}; break;
        case 7: layout = {fl, c, fr, sl7, sr7, bc, lfe}; break;
        default: layout = {fl, c, fr, sl7, sr7, bl7, br7, lfe}; break;
    }
    return layout;
}

/// RIFF WAVE: channels follow the speaker mask's set bits in bit order (WAVE_FORMAT_EXTENSIBLE);
/// without a mask, the default order for the count (Windows' KSAUDIO_SPEAKER_* layouts).
[[nodiscard]] inline SourceLayout wave_layout(uint32_t channels, uint32_t mask) noexcept {
    if (mask == 0) {
        switch (channels) {
            case 1: mask = 0x4; break;    // front centre
            case 2: mask = 0x3; break;    // stereo
            case 3: mask = 0x7; break;    // FL FR FC
            case 4: mask = 0x33; break;   // quad
            case 5: mask = 0x37; break;   // FL FR FC BL BR
            case 6: mask = 0x3F; break;   // 5.1
            case 7: mask = 0x13F; break;  // 5.1 + back centre
            default: mask = 0x63F; break; // 7.1
        }
    }
    // With a single surround pair (called back or side), it sits at 110 degrees; with both pairs
    // the sides are at 90 and the backs at 150 (ITU-R BS.2051).
    const bool both_pairs = (mask & 0x30) != 0 && (mask & 0x600) != 0;
    const float back = both_pairs ? 150.0f : kSurroundAzimuth;
    const float side = both_pairs ? 90.0f : kSurroundAzimuth;
    const std::array<SourceChannel, 18> bits = {{
        {-30.0f},                // FRONT_LEFT
        {30.0f},                 // FRONT_RIGHT
        {0.0f},                  // FRONT_CENTER
        {0.0f, 0.0f, true},      // LOW_FREQUENCY
        {-back},                 // BACK_LEFT
        {back},                  // BACK_RIGHT
        {-15.0f},                // FRONT_LEFT_OF_CENTER
        {15.0f},                 // FRONT_RIGHT_OF_CENTER
        {180.0f},                // BACK_CENTER
        {-side},                 // SIDE_LEFT
        {side},                  // SIDE_RIGHT
        {0.0f, 90.0f},           // TOP_CENTER
        {-45.0f, 45.0f},         // TOP_FRONT_LEFT
        {0.0f, 45.0f},           // TOP_FRONT_CENTER
        {45.0f, 45.0f},          // TOP_FRONT_RIGHT
        {-135.0f, 45.0f},        // TOP_BACK_LEFT
        {180.0f, 45.0f},         // TOP_BACK_CENTER
        {135.0f, 45.0f},         // TOP_BACK_RIGHT
    }};
    SourceLayout layout{};  // channels beyond the mask's speakers play from the front
    uint32_t c = 0;
    for (uint32_t bit = 0; bit < bits.size() && c < channels && c < layout.size(); ++bit) {
        if ((mask & (1u << bit)) != 0) {
            layout[c++] = bits[bit];
        }
    }
    return layout;
}

}  // namespace vsa
