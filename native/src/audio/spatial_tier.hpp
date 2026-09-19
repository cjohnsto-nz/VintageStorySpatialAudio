#pragma once

#include <cstdint>

namespace vsa {

/// How a positional voice is spatialised.
enum class SpatialTier : uint8_t {
    /// Amplitude panning to the output's speakers.
    Panned,
    /// Its own HRTF (headphones, within the binaural budget).
    Binaural,
    /// Encoded into the world ambisonic bus, decoded once per block (headphones, beyond the budget).
    Ambisonic,
};

}  // namespace vsa
