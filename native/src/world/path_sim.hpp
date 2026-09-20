#pragma once

#include "audio/listener_pose.hpp"
#include "core/latest_value.hpp"
#include "steam/ipl_handle.hpp"
#include "vsaudio.h"
#include "world/path_baker.hpp"
#include "world/path_channel.hpp"

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

struct PathSimSettings {
    /// Simulations per second while a device plays.
    uint32_t rate_hz = 10;
    /// Sources given paths per run at most (the loudest that want one).
    uint32_t max_sources = 16;
};

/// One leg of a path Steam Audio considered in the latest run (its visualisation callback), in
/// scene coordinates.
struct PathSegment {
    float from[3] = {};
    float to[3] = {};
    bool occluded = false;
};

struct PathSimStats {
    uint64_t ticks = 0;
    double last_tick_ms = 0.0;
    double max_tick_ms = 0.0;
    uint32_t wanted = 0;     // sources that asked for a path in the latest run
    uint32_t simulated = 0;  // of those, run (at most max_sources)
    uint32_t found = 0;      // of those, with a path
    uint64_t batch_id = 0;   // the probe batch in use (0: none yet)
    float listener[3] = {};
};

/// The pathing simulation (Phase 7, ADR 0014): Steam Audio's baked pathing for the voices whose
/// straight path is blocked, in the baker's current probe batch. Each run: the batch is taken
/// over from the baker if it changed (added to the simulator and committed; the old one released
/// after), the wanted sources are set (the loudest `max_sources`; a fresh Steam Audio source
/// each run, since one that finds no path keeps its last), the paths are looked up and
/// validated against the live scene (`enableValidation`, `findAlternatePaths`), and the path
/// effect parameters go out through the channel. Runs on its own thread at `rate_hz` while a
/// device plays, or synchronously from offline rendering.
class PathSimulator {
public:
    PathSimulator(const steam::SteamContext& steam, WorldScene& scene, PathBaker& baker, PathChannel& channel,
                  const PathSimSettings& settings, const PathBakeSettings& bake);
    ~PathSimulator();

    PathSimulator(const PathSimulator&) = delete;
    PathSimulator& operator=(const PathSimulator&) = delete;

    void set_listener(const ListenerPose& pose) noexcept { listener_.publish(pose); }
    void set_threaded(bool threaded);
    /// Debugging: no alternates to a blocked baked path, so the legs reported are the legs heard.
    void set_baked_only(bool on) noexcept { baked_only_.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool threaded() const noexcept { return thread_.joinable(); }
    void offline_tick(double seconds);
    /// One run. Not concurrent with itself.
    void tick();

    [[nodiscard]] PathSimStats stats() const;
    [[nodiscard]] std::vector<PathSegment> segments() const;

private:
    std::atomic<bool> baked_only_{false};
    struct Source {
        steam::Source handle;
        bool added = false;
        uint32_t generation = 0;
    };
    struct Wanted {
        uint32_t set;
        float level;
    };

    void thread_main();
    static void IPLCALL on_segment(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* user);

    const steam::SteamContext& steam_;
    WorldScene& scene_;
    PathBaker& baker_;
    PathChannel& channel_;
    const PathSimSettings settings_;
    const PathBakeSettings bake_;
    steam::Simulator simulator_;
    std::shared_ptr<const PathBatch> batch_;  // the one added to the simulator
    std::vector<Source> sources_;
    std::vector<steam::Source> retired_;  // removed this run, released after the commit
    std::vector<Wanted> wanted_;
    std::vector<bool> run_;  // per set, this run
    std::vector<PathSegment> segments_;  // this run's, reused
    LatestValue<ListenerPose> listener_;
    double next_offline_ = 0.0;
    bool have_ticked_ = false;

    std::mutex thread_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;

    mutable std::mutex debug_mutex_;
    std::vector<PathSegment> debug_;
    PathSimStats stats_;
};

}  // namespace vsa::world
