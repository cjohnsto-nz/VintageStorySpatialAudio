#pragma once

#include "audio/spatial_tier.hpp"
#include "dsp/gain_ramp.hpp"
#include "dsp/high_shelf.hpp"
#include "dsp/resampler.hpp"
#include "vsaudio.h"

#include <atomic>
#include <cstdint>

namespace vsa {

class Asset;
class Stream;

/// Commands from API threads to the render thread. POD; travels through an SPSC ring.
enum class Op : uint32_t {
    Activate,
    Start,
    Pause,
    Stop,
    SetGain,
    SetPitch,
    SetLooping,
    Seek,
    Fade,
    Release,
    SetPosition,
    SetLowpass,
    SetBusGain,
    SetMasterGain,
    SetRenderMode,
    SetReflectionGain,
    SetReflectionMix,  // value: early reflections' gain, seconds: the tail's
};

struct Command {
    Op op;
    /// Voice slot, or the bus index for SetBusGain.
    uint32_t slot;
    /// Start/Pause/Stop: the voice's state-command sequence number this command completes.
    uint32_t seq;
    /// Stop/Seek: the voice's position-command sequence number this command completes.
    uint32_t seek_seq;
    uint32_t flags;
    float value;
    float seconds;
    double position;
    uint64_t token;
    /// SetPosition: the position (the spatial mode travels in `flags`).
    float vec[3];
};

/// Render-thread-only state of a voice.
struct RenderVoice {
    enum Pending : uint8_t { kNone = 0, kPause = 1, kStop = 2, kSeek = 4, kRelease = 8 };

    bool active = false;
    uint32_t active_index = 0;
    uint32_t state = VSA_VOICE_STOPPED;

    dsp::SourcePosition pos;
    bool has_looped = false;
    float pitch = 1.0f;
    bool looping = false;

    dsp::GainRamp gain;
    bool fade_active = false;
    bool fade_stop = false;
    uint64_t fade_token = 0;

    // Declick envelope: moves linearly between 0 and 1; pending actions run when it reaches 0.
    float env = 1.0f;
    float env_target = 1.0f;
    uint8_t pending = kNone;
    double pending_seek_seconds = 0.0;

    bool underrun = false;

    // Positioning.
    uint32_t spatial = VSA_SPATIAL_NONE;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float min_distance = 1.0f;
    /// Spatial effect set held while real, -1 otherwise.
    int effect_set = -1;
    /// Inaudible: position advances, nothing is rendered.
    bool is_virtual = false;
    /// Estimated output level from the last block (gain x bus x master x distance), times what
    /// walls let through.
    float level = 0.0f;
    /// The same without walls: what decides who gets reflections of their own.
    float open_level = 0.0f;
    /// The place (a reflection slot >= 1) whose simulation its reflections come from, or -1
    /// (ADR 0012), and that place's generation when it joined: the place may have been taken over.
    int reflection_slot = -1;
    uint32_t place_generation = 0;
    /// Panned (speakers), its own HRTF (headphones, within the binaural budget) or the world
    /// ambisonic bus (headphones, beyond it).
    SpatialTier tier = SpatialTier::Panned;
    /// Blocks held back waiting for the first direct-simulation result (a sound starting behind a
    /// wall must not play unoccluded meanwhile).
    uint16_t direct_hold = 0;
    /// Has produced audio since it was last started (only a voice's onset waits for a result).
    bool sounded = false;

    dsp::HighShelf shelf;
};

/// One voice slot. Handles are (generation << 32 | slot index).
///
/// Ownership of the fields:
///  - atomics: shared, see the comments;
///  - "config" fields: written by the API thread before it posts Activate (the ring's release
///    publishes them), then read by the render thread; cleared by the worker when the slot is
///    returned to the free list;
///  - `render`: render thread only.
struct VoiceSlot {
    // API: validity. `generation` is bumped on release, so stale handles stop matching at once.
    std::atomic<uint32_t> generation{1};
    std::atomic<bool> in_use{false};

    // State as issued (API) and as applied (render). While applied_seq != cmd_seq a command is in
    // flight and requested_state is the truth; otherwise render_state is (it also changes on its
    // own, e.g. when a voice ends).
    std::atomic<uint32_t> requested_state{VSA_VOICE_STOPPED};
    std::atomic<uint32_t> cmd_seq{0};
    std::atomic<uint32_t> render_state{VSA_VOICE_STOPPED};
    std::atomic<uint32_t> applied_seq{0};

    // Position, same scheme for seeks (and stops, which rewind).
    std::atomic<double> requested_position{0.0};
    std::atomic<uint32_t> seek_seq{0};
    std::atomic<uint32_t> seek_applied{0};
    std::atomic<double> position{0.0};

    // Config.
    vsa_voice handle = 0;
    Asset* asset = nullptr;
    Stream* stream = nullptr;
    uint32_t bus = 0;
    float initial_gain = 1.0f;
    float initial_pitch = 1.0f;
    bool initial_looping = false;
    uint32_t initial_spatial = VSA_SPATIAL_NONE;
    float initial_position[3] = {0.0f, 0.0f, 0.0f};
    float initial_min_distance = 1.0f;

    // What this voice sounded like in the latest block, and by which way it reached the
    // listener, for the sound inspector (".steamaudio scene sounds"). Written by the render
    // thread when the engine is asked for it, read by the API: a torn read is a wrong decimal
    // in a debug view, and the render thread must not lock for it.
    std::atomic<float> heard_db{-200.0f};
    std::atomic<float> direct_db{-200.0f};
    std::atomic<float> path_db{-200.0f};
    std::atomic<float> reflection_db{-200.0f};
    std::atomic<float> heard_distance{0.0f};
    std::atomic<uint32_t> heard_flags{0};
    std::atomic<uint64_t> heard_block{0};

    RenderVoice render;
};

[[nodiscard]] constexpr vsa_voice make_handle(uint32_t slot, uint32_t generation) noexcept {
    return (static_cast<uint64_t>(generation) << 32) | slot;
}
[[nodiscard]] constexpr uint32_t handle_slot(vsa_voice handle) noexcept { return static_cast<uint32_t>(handle); }
[[nodiscard]] constexpr uint32_t handle_generation(vsa_voice handle) noexcept {
    return static_cast<uint32_t>(handle >> 32);
}

}  // namespace vsa
