#pragma once

#include "steam/ipl_handle.hpp"
#include "vsaudio.h"

#include <cstdint>
#include <vector>

namespace vsa {

namespace steam {
class SteamContext;
}

/// Where a positional voice is relative to the listener, computed once per block.
struct SpatialParams {
    /// Unit vector in listener space (+x right, +y up, -z forward).
    float direction[3] = {0.0f, 0.0f, -1.0f};
    /// 1 = fully spatialised; lower values blend towards a centred, unspatialised signal
    /// (sources at the listener's head have no meaningful direction).
    float spatial_blend = 1.0f;
    /// Inverse-distance attenuation, 0..1.
    float distance_gain = 1.0f;
    /// Steam Audio 3-band air absorption, 0..1 each.
    float air_absorption[3] = {1.0f, 1.0f, 1.0f};
};

/// Steam Audio rendering for positional voices: a fixed pool of effect sets (direct effect for
/// distance attenuation and air absorption, binaural effect for headphones, panning effect for
/// speakers) sharing one HRTF.
///
/// prepare() allocates and must not run while anything renders. acquire/release/reset/render are
/// for the render thread only and never allocate.
class SpatialRenderer {
public:
    SpatialRenderer(const steam::SteamContext& steam, uint32_t pool_size);

    SpatialRenderer(const SpatialRenderer&) = delete;
    SpatialRenderer& operator=(const SpatialRenderer&) = delete;

    /// (Re)creates the HRTF and every effect set for this rate, block size and output channel count
    /// (panning targets stereo, quad, 5.1 or 7.1 for 2/4/6/8 channels). Throws vsa::Error.
    void prepare(uint32_t sample_rate, uint32_t block_frames, uint32_t channels);

    /// Channels the panning effect writes (the output layout); binaural always writes 2.
    [[nodiscard]] uint32_t speaker_channels() const noexcept { return speaker_channels_; }

    /// An effect set index, or -1 when all are in use.
    int acquire() noexcept;
    void release(int set) noexcept;
    /// Clears a set's filter state (use when a set changes voices).
    void reset(int set) noexcept;
    /// Clears only the binaural (true) or panning (false) state: for switching a voice between them.
    void reset_spatialiser(int set, bool binaural) noexcept;
    void reset_all() noexcept;

    [[nodiscard]] uint32_t pool_size() const noexcept { return pool_size_; }
    [[nodiscard]] uint32_t in_use() const noexcept { return pool_size_ - free_count_; }

    /// Renders one block: `mono` (modified in place by the direct effect) into `out`, which must
    /// have speaker_channels() channels for VSA_RENDER_SPEAKERS and 2 for headphones (overwritten,
    /// in the engine's channel order: channels 0/1 are front left/right).
    void render(int set, vsa_render_mode mode, const SpatialParams& params, float* mono, float* const* out) noexcept;

private:
    struct EffectSet {
        steam::DirectEffect direct;
        steam::BinauralEffect binaural;
        steam::PanningEffect panning;
    };

    const steam::SteamContext& steam_;
    uint32_t pool_size_;
    uint32_t frames_ = 0;
    uint32_t speaker_channels_ = 2;
    steam::Hrtf hrtf_;
    std::vector<EffectSet> sets_;
    std::vector<int> free_;
    uint32_t free_count_ = 0;
};

}  // namespace vsa
