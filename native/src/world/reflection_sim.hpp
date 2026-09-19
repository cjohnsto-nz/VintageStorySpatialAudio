#pragma once

#include "audio/listener_pose.hpp"
#include "core/latest_value.hpp"
#include "steam/ipl_handle.hpp"
#include "vsaudio.h"
#include "world/reflection_channel.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vsa::steam {
class SteamContext;
}

namespace vsa::world {

class WorldScene;

/// Reflection simulation quality (PLAN 5.4); validated by the engine. The defaults are the
/// Balanced preset. A run's cost is mostly Steam Audio rebuilding every source's impulse response
/// (about 4 ms per source per second of response at order 2), not the rays: sources, duration and
/// order are the expensive settings.
struct ReflectionSettings {
    /// Voices with reflections of their own; the rest share the listener's reverb.
    uint32_t sources = 8;
    uint32_t rays = 4096;
    uint32_t bounces = 16;
    /// Impulse response length, seconds.
    float duration = 1.0f;
    /// Ambisonic order of the reflections, 1..3.
    uint32_t order = 2;
    /// Simulation updates per second (at most: a slow run delays the next).
    uint32_t rate_hz = 10;
    /// Steam Audio's own worker threads for a run.
    uint32_t threads = 2;
    /// Seconds of each impulse response rendered by convolution; the diffuse tail after it is
    /// parametric (ADR 0009).
    float transition = 0.1f;
    float irradiance_min_distance = 1.0f;
};

/// One slot's latest results, for debugging views.
struct ReflectionSlotDebug {
    uint32_t slot = 0;
    vsa_voice voice = 0;               // 0 for the listener's reverb
    float position[3] = {};            // simulated from (scene coordinates)
    float reverb_times[3] = {};        // RT60 per band, seconds
    float eq[3] = {};                  // level of the tail's start per band
    int32_t delay = 0;                 // samples before the tail starts
};

struct ReflectionStats {
    uint32_t slots = 0;   // per-voice slots (the listener's reverb not counted)
    uint32_t active = 0;  // per-voice slots simulated in the latest tick
    uint64_t ticks = 0;
    double last_tick_ms = 0.0;
    double max_tick_ms = 0.0;
    double simulate_ms = 0.0;  // the latest tick's Steam Audio run
    float listener_reverb_times[3] = {};
    float listener[3] = {};
    ReflectionSettings settings;
};

/// The reflection simulation (Phase 6, ADR 0009): Steam Audio's real-time ray-traced reflections
/// against the world scene, with the hybrid reverb parameters, for a fixed pool of sources: slot
/// 0 at the listener (the reverb every other sound shares) and one per voice the render thread
/// gives a slot. Sources and their impulse-response buffers live as long as this object, so the
/// render thread's effects can hold on to them. Runs on its own thread at up to `rate_hz` while a
/// device plays, or synchronously from offline rendering (deterministic).
///
/// Its sample rate and block size are those of the output (the impulse responses are partitioned
/// for the render block): the engine rebuilds it when they change.
class ReflectionSimulator {
public:
    ReflectionSimulator(const steam::SteamContext& steam, WorldScene& scene, ReflectionChannel& channel,
                        const ReflectionSettings& settings, uint32_t sample_rate, uint32_t frame_size);
    ~ReflectionSimulator();

    ReflectionSimulator(const ReflectionSimulator&) = delete;
    ReflectionSimulator& operator=(const ReflectionSimulator&) = delete;

    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] uint32_t frame_size() const noexcept { return frame_size_; }
    [[nodiscard]] const ReflectionSettings& settings() const noexcept { return settings_; }
    /// Slots including the listener's (slot 0).
    [[nodiscard]] uint32_t slot_count() const noexcept { return static_cast<uint32_t>(sources_.size()); }
    /// A slot's impulse response, for its reflection effect (valid while this object lives).
    [[nodiscard]] IPLReflectionEffectIR ir(uint32_t slot) const noexcept { return sources_[slot].ir; }
    [[nodiscard]] uint32_t ir_channels() const noexcept { return ir_channels_; }

    /// The listener, in scene coordinates.
    void set_listener(const ListenerPose& pose) noexcept { listener_.publish(pose); }

    void set_threaded(bool threaded);
    [[nodiscard]] bool threaded() const noexcept { return thread_.joinable(); }
    /// Offline rendering: called before each block with the output's elapsed time.
    void offline_tick(double seconds);
    /// One simulation run. Not concurrent with itself.
    void tick();

    [[nodiscard]] ReflectionStats stats() const;
    [[nodiscard]] std::vector<ReflectionSlotDebug> slots() const;

private:
    struct Source {
        steam::Source handle;
        IPLReflectionEffectIR ir = nullptr;
        bool enabled = false;
        uint32_t generation = 0;
        uint32_t simulated = 0;    // the generation its latest results are for
        ReflectionSlotDebug last;  // for the debugging view while it waits its turn
        double held[3] = {};       // where it is simulated from (see kHoldMetres)
        uint32_t held_generation = 0;
    };

    /// Voice slots simulated per run: at most half of them (at least 4), round-robin, so each
    /// run costs less and fewer impulse responses change at once on the render thread.
    [[nodiscard]] uint32_t per_run() const noexcept { return std::max(4u, (settings_.sources + 1) / 2); }

    void thread_main();

    const steam::SteamContext& steam_;
    WorldScene& scene_;
    ReflectionChannel& channel_;
    const ReflectionSettings settings_;
    const uint32_t sample_rate_;
    const uint32_t frame_size_;
    uint32_t ir_channels_ = 0;
    steam::Simulator simulator_;
    std::vector<Source> sources_;
    uint32_t cursor_ = 1;  // where the round-robin over voice slots continues
    double held_listener_[3] = {};
    bool listener_held_ = false;
    LatestValue<ListenerPose> listener_;
    double next_offline_ = 0.0;
    bool have_ticked_ = false;

    std::mutex thread_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;

    mutable std::mutex debug_mutex_;
    std::vector<ReflectionSlotDebug> debug_;
    ReflectionStats stats_;
};

}  // namespace vsa::world
