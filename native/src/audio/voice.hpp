#pragma once

#include "dsp/gain_ramp.hpp"
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
    SetBusGain,
    SetMasterGain,
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
