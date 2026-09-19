#pragma once

#include "audio/listener_pose.hpp"
#include "world/direct_channel.hpp"
#include "core/latest_value.hpp"
#include "steam/ipl_handle.hpp"
#include "vsaudio.h"
#include "world/transmission.hpp"

#include <atomic>
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

/// One simulated source's latest results, for debugging views.
struct SourceDebug {
    vsa_voice voice = 0;
    float position[3] = {};            // scene coordinates, as the render thread placed it
    float simulated_position[3] = {};  // after escaping the block it sits in
    bool escaped = false;
    float occlusion = 1.0f;
    float transmission[3] = {1.0f, 1.0f, 1.0f};
    float solid_metres = 0.0f;
    uint32_t crossings = 0;
};

struct SimulationStats {
    uint32_t sources = 0;
    uint64_t ticks = 0;
    double last_tick_ms = 0.0;
    double max_tick_ms = 0.0;
    double occlusion_ms = 0.0;
    double transmission_ms = 0.0;
    uint32_t rate_hz = 0;
    uint32_t occlusion_samples = 0;
    float listener[3] = {};  // scene coordinates, as simulated
    int32_t origin[3] = {};
};

/// The direct-sound simulation (Phase 5): for every voice with an effect set, Steam Audio's
/// volumetric occlusion against the world scene, and our own voxel transmission along the
/// listener-source line (ADR 0004). Runs on its own thread at `rate_hz` while a device plays, or
/// synchronously from offline rendering (deterministic). Results reach the render thread through
/// a DirectChannel; the render thread smooths them.
///
/// Where the centre line is clear but the source is partly occluded (at a corner), the occluded
/// part passes through `kEdgeTransmission` rather than a wall.
class DirectSimulator {
public:
    DirectSimulator(const steam::SteamContext& steam, WorldScene& scene, DirectChannel& channel,
                    uint32_t occlusion_samples, uint32_t rate_hz);
    ~DirectSimulator();

    DirectSimulator(const DirectSimulator&) = delete;
    DirectSimulator& operator=(const DirectSimulator&) = delete;

    /// The listener, in scene coordinates (published by the engine alongside the mixer's copy).
    void set_listener(const ListenerPose& pose) noexcept { listener_.publish(pose); }

    /// Runs the simulation on its own thread (device output) or not (offline output).
    void set_threaded(bool threaded);
    /// Offline rendering: called before each block with the output's elapsed time; runs a tick
    /// whenever a period has passed (the first call always ticks).
    void offline_tick(double seconds);
    /// One simulation step. Not concurrent with itself (the thread or offline ticks, never both).
    void tick();

    [[nodiscard]] SimulationStats stats() const;
    [[nodiscard]] std::vector<SourceDebug> sources() const;

    /// Amplitude through the occluded part of a partly occluded source whose centre line is clear.
    static constexpr float kEdgeTransmission[3] = {0.5f, 0.35f, 0.25f};

private:
    struct Source {
        steam::Source handle;
        bool added = false;
        uint32_t generation = 0;
    };
    struct Active {
        uint32_t set;
        uint32_t generation;
        SourceDebug debug;
        double world[3];
    };

    void thread_main();

    const steam::SteamContext& steam_;
    WorldScene& scene_;
    DirectChannel& channel_;
    const uint32_t samples_;
    const uint32_t rate_hz_;
    steam::Simulator simulator_;
    std::vector<Source> sources_;
    std::vector<Active> active_;         // per tick, reused
    uint64_t committed_scene_ = ~0ull;   // the scene commit the simulator last picked up
    LatestValue<ListenerPose> listener_;
    double next_offline_ = 0.0;
    bool have_ticked_ = false;

    std::mutex thread_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;

    mutable std::mutex debug_mutex_;
    std::vector<SourceDebug> debug_;
    SimulationStats stats_;
};

}  // namespace vsa::world
