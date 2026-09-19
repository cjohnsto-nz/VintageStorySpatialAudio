#pragma once

#include "audio/spatial_tier.hpp"
#include "audio/vbap.hpp"
#include "dsp/spherical_harmonics.hpp"
#include "steam/ipl_handle.hpp"
#include "vsaudio.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
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
    /// Inverse-distance attenuation, 0..1, and the distance itself (metres).
    float distance_gain = 1.0f;
    float distance = 0.0f;
    /// Steam Audio 3-band air absorption, 0..1 each.
    float air_absorption[3] = {1.0f, 1.0f, 1.0f};
    /// From the direct simulation (Phase 5): the visible fraction of the source, and the amplitude
    /// per band of what passes through the geometry in the way.
    float occlusion = 1.0f;
    float transmission[3] = {1.0f, 1.0f, 1.0f};
    /// Unit listener-to-source direction in world space (for the world ambisonic bus).
    float world_direction[3] = {0.0f, 0.0f, -1.0f};
};

/// The listener's orientation (and position) for decoding the world ambisonic bus.
struct Orientation {
    float right[3];
    float up[3];
    float ahead[3];
    float origin[3];
};

/// Steam Audio rendering for positional voices: a fixed pool of effect sets (direct effect for
/// distance attenuation and air absorption, binaural effect for headphones, panning effect for
/// speakers) sharing one HRTF.
///
/// prepare() allocates and must not run while anything renders. acquire/release/reset/render are
/// for the render thread only and never allocate.
class SpatialRenderer {
public:
    /// `sofa_path`: an HRTF to load instead of Steam Audio's default (empty: the default).
    SpatialRenderer(const steam::SteamContext& steam, uint32_t pool_size, std::string sofa_path = {});

    SpatialRenderer(const SpatialRenderer&) = delete;
    SpatialRenderer& operator=(const SpatialRenderer&) = delete;

    /// (Re)creates the HRTF and every effect set for this rate, block size and output channel count
    /// (panning targets stereo, quad, 5.1 or 7.1 for 2/4/6/8 channels with Steam Audio's panning
    /// effect, and 7.1.4 for 12 with our own 3D VBAP). Throws vsa::Error.
    void prepare(uint32_t sample_rate, uint32_t block_frames, uint32_t channels);

    /// Channels the panning effect writes (the output layout); binaural always writes 2.
    [[nodiscard]] uint32_t speaker_channels() const noexcept { return speaker_channels_; }

    /// An effect set index, or -1 when all are in use.
    int acquire() noexcept;
    void release(int set) noexcept;
    /// Clears a set's filter state (use when a set changes voices).
    void reset(int set) noexcept;
    /// The set's path effect (Phase 7): `mono` (with every gain) rendered as the paths Steam
    /// Audio found (`sh`: 4 order-1 world-space coefficients carrying their attenuation; `eq` per
    /// band), into 4 world-space Ambisonic channels (overwritten).
    void render_path(int set, const float eq[3], const float sh[4], const float* mono, float* const* out4) noexcept;

    /// Clears the state of one tier's effect: for switching a voice between tiers.
    void reset_tier(int set, SpatialTier tier) noexcept;
    void reset_all() noexcept;

    [[nodiscard]] uint32_t pool_size() const noexcept { return pool_size_; }
    [[nodiscard]] uint32_t in_use() const noexcept { return pool_size_ - free_count_; }

    /// Renders one block: `mono` (modified in place by the direct effect) into `out`, which must
    /// have speaker_channels() channels for VSA_RENDER_SPEAKERS and 2 for headphones (overwritten,
    /// in the engine's channel order: channels 0/1 are front left/right).
    void render(int set, vsa_render_mode mode, const SpatialParams& params, float* mono, float* const* out) noexcept;

    // ---- World ambisonic bus (order kAmbisonicOrder) ----

    static constexpr int kAmbisonicOrder = 3;
    static constexpr uint32_t kAmbisonicChannels = (kAmbisonicOrder + 1) * (kAmbisonicOrder + 1);
    static_assert(kAmbisonicChannels == dsp::kSh3Channels);

    /// Start of a block: clears the bus.
    void begin_block() noexcept;
    /// Applies the set's direct effect to `mono` (in place), then encodes it towards
    /// params.world_direction (coefficients interpolated across the block from the set's
    /// previous direction) and adds it to the bus. `mono` should already carry every gain.
    /// Below full spatial_blend the directional components fade out, leaving the omnidirectional
    /// one: a source at the head is heard centred.
    void encode(int set, const SpatialParams& params, float* mono) noexcept;
    /// Adds another world-space Ambisonic signal (Steam Audio's convention; `channels` <= 16, the
    /// lowest orders) to this block's bus: the reflections share the binaural decode.
    void add_ambisonic(const float* const* in, uint32_t channels) noexcept;
    /// Decodes the bus binaurally for this listener orientation into `left`/`right` (overwritten).
    /// Returns false (and writes nothing) when the bus and the decoder's tail are silent.
    bool decode(const Orientation& orientation, float* left, float* right) noexcept;

    // ---- Head bus (order kAmbisonicOrder, listener space): beds on headphones ----

    /// Adds `mono` (already carrying every gain) from a fixed listener-space direction (+x right,
    /// +y up, -z ahead) to this block's head bus: a bed channel, which turns with the head as a
    /// speaker would. Directions do not move, so nothing is ramped.
    void encode_head(const float direction[3], const float* mono) noexcept;
    /// Decodes the head bus binaurally, facing ahead, into `left`/`right` (overwritten). Returns
    /// false (and writes nothing) when the bus and the decoder's tail are silent.
    bool decode_head(float* left, float* right) noexcept;

private:
    struct EffectSet {
        steam::DirectEffect direct;
        steam::BinauralEffect binaural;
        steam::PanningEffect panning;
        steam::PathEffect path;
        // VBAP (7.1.4): last block's speaker gains, the start of this block's ramp.
        std::array<float, kMaxOutputChannels> pan{};
        bool pan_ready = false;
        // Ambisonic encoding: last block's coefficients, the start of this block's ramp.
        std::array<float, dsp::kSh3Channels> sh{};
        bool sh_ready = false;
    };

    void apply_direct(EffectSet& set, const SpatialParams& params, float* mono) noexcept;
    void render_vbap(EffectSet& set, const SpatialParams& params, const float* mono, float* const* out) noexcept;

    const steam::SteamContext& steam_;
    uint32_t pool_size_;
    std::string sofa_path_;
    uint32_t frames_ = 0;
    uint32_t speaker_channels_ = 2;
    steam::Hrtf hrtf_;
    std::unique_ptr<Vbap> vbap_;  // layouts with heights
    std::vector<EffectSet> sets_;
    std::vector<int> free_;
    uint32_t free_count_ = 0;

    steam::AmbisonicsDecodeEffect decoder_;
    std::vector<float> bus_storage_;
    std::array<float*, kAmbisonicChannels> bus_{};
    std::vector<float> ramp_;  // (j + 1) / frames: the per-frame weight of a coefficient change
    bool bus_used_ = false;
    // The highest order in the bus this block (voices are order 3; the reflections may be lower),
    // and the last block's: the decoder runs at the higher of the two, so a tail rings out.
    int bus_order_ = 0;
    int last_order_ = 0;
    // Blocks the decoder keeps running after the bus falls silent, to let its convolution tail out.
    uint32_t tail_blocks_ = 0;

    // The head bus: its own decoder (its convolution state is not the world bus's).
    steam::AmbisonicsDecodeEffect head_decoder_;
    std::vector<float> head_storage_;
    std::array<float*, kAmbisonicChannels> head_bus_{};
    bool head_used_ = false;
    uint32_t head_tail_blocks_ = 0;
};

}  // namespace vsa
