#pragma once

#include <cstddef>

namespace vsa::dsp {

/// Channels of an order-3 Ambisonic signal.
inline constexpr std::size_t kSh3Channels = 16;

/// Real spherical harmonics up to order 3 for a unit direction given in Steam Audio's coordinates
/// (+x right, +y up, -z ahead), in Steam Audio's own Ambisonic convention: ACN channel order,
/// orthonormal (N3D / sqrt(4 pi)) normalisation. Matches iplAmbisonicsEncodeEffect's steady-state
/// output exactly (test_spherical_harmonics.cpp), so the result feeds Steam Audio's decoder.
inline void sh_order3(const float direction[3], float out[kSh3Channels]) noexcept {
    // Ambisonic axes: x ahead, y left, z up.
    const float x = -direction[2];
    const float y = -direction[0];
    const float z = direction[1];
    const float x2 = x * x;
    const float y2 = y * y;
    const float z2 = z * z;

    out[0] = 0.28209479f;

    out[1] = 0.48860251f * y;
    out[2] = 0.48860251f * z;
    out[3] = 0.48860251f * x;

    out[4] = 1.09254843f * x * y;
    out[5] = 1.09254843f * y * z;
    out[6] = 0.31539157f * (3.0f * z2 - 1.0f);
    out[7] = 1.09254843f * x * z;
    out[8] = 0.54627422f * (x2 - y2);

    out[9] = 0.59004359f * y * (3.0f * x2 - y2);
    out[10] = 2.89061144f * x * y * z;
    out[11] = 0.45704580f * y * (5.0f * z2 - 1.0f);
    out[12] = 0.37317633f * z * (5.0f * z2 - 3.0f);
    out[13] = 0.45704580f * x * (5.0f * z2 - 1.0f);
    out[14] = 1.44530572f * z * (x2 - y2);
    out[15] = 0.59004359f * x * (x2 - 3.0f * y2);
}

}  // namespace vsa::dsp
