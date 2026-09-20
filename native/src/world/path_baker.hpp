#pragma once

#include "audio/listener_pose.hpp"
#include "core/latest_value.hpp"
#include "steam/ipl_handle.hpp"
#include "world/voxel.hpp"

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

/// How pathing data is baked (ADR 0014); validated by the engine.
struct PathBakeSettings {
    /// The box baked round the listener: blocks across (x and z) and high (y).
    uint32_t range = 64;
    uint32_t height = 64;
    /// Probes this far apart, this high above every floor. The spacing is widened when the
    /// terrain would otherwise put more than `max_probes` in the box.
    float spacing = 2.5f;
    float probe_height = 1.6f;
    /// Probes to bake at most (ADR 0015). A bake costs about probes^2.2, and how many probes a
    /// box holds is up to the terrain: without a budget, open ground takes tens of seconds.
    uint32_t max_probes = 1200;
    /// Point samples per probe when testing whether two probes see each other (1: fast; more:
    /// robust to thin gaps).
    uint32_t vis_samples = 1;
    float vis_radius = 1.0f;
    float vis_threshold = 0.1f;
    /// Probes further apart than this are never neighbours; paths longer than this do not exist.
    float vis_range = 32.0f;
    float path_range = 64.0f;
    /// Seconds after the last change to a chunk in the box before it is baked again.
    double rebake_seconds = 3.0;
    uint32_t threads = 1;
};

/// A baked probe batch: the pathing data for a box of the world, in the scene coordinates of
/// the origin it was baked under.
struct PathBatch {
    steam::ProbeBatch batch;
    int32_t origin[3] = {};
    /// The box, world block coordinates.
    double min[3] = {};
    double max[3] = {};
    uint32_t probes = 0;
    double bake_ms = 0.0;
    uint64_t id = 0;
};

struct PathBakeStats {
    bool baking = false;
    uint64_t bakes = 0;
    /// Bakes abandoned because the listener had moved on before they finished.
    uint64_t cancelled = 0;
    double last_bake_ms = 0.0;
    double max_bake_ms = 0.0;
    uint32_t probes = 0;   // of the current batch
    float spacing = 0.0f;  // it was baked with (wider than settings when the budget bit)
    double centre[3] = {};  // of the current batch's box (world)
    bool dirty = false;    // a bake is due (chunks changed, the listener left the middle)
};

/// Bakes Steam Audio pathing data for one box of the world round the listener (ADR 0014), on
/// its own thread while a device plays, or synchronously from offline rendering. A bake works on
/// a snapshot of the scene, so scene edits are never held up; the batch it produces is picked up
/// by the pathing simulation (`current()`), and the old one lives until that has let go of it.
///
/// It bakes again when the listener has left the middle third of the box, when a chunk in the
/// box changed (`rebake_seconds` after the last change), or when the scene origin moved.
class PathBaker {
public:
    PathBaker(const steam::SteamContext& steam, WorldScene& scene, const PathBakeSettings& settings);
    ~PathBaker();

    PathBaker(const PathBaker&) = delete;
    PathBaker& operator=(const PathBaker&) = delete;

    [[nodiscard]] const PathBakeSettings& settings() const noexcept { return settings_; }
    /// The listener, in scene coordinates. Called every frame: it also abandons a bake whose
    /// box the listener has already left, which would otherwise finish and be thrown away.
    void set_listener(const ListenerPose& pose) noexcept;

    void set_threaded(bool threaded);
    [[nodiscard]] bool threaded() const noexcept { return thread_.joinable(); }
    /// Offline rendering: bakes now if a bake is due (`seconds` is the output's clock).
    void offline_tick(double seconds);
    /// The latest baked batch (null before the first).
    [[nodiscard]] std::shared_ptr<const PathBatch> current() const;
    [[nodiscard]] PathBakeStats stats() const;

private:
    struct Box {
        double min[3];
        double max[3];
    };

    void thread_main();
    /// Whether a bake is due at `now` (seconds on the caller's clock), and the box it would be.
    [[nodiscard]] bool due(double now, Box& box);
    void bake(const Box& box);
    /// Probes for `box`, widening the spacing until there are at most `max_probes` of them.
    [[nodiscard]] steam::ProbeArray generate(const Box& box, const IPLScene scene, const int32_t origin[3],
                                             uint32_t& probes, float& spacing) const;
    [[nodiscard]] std::vector<ChunkKey> keys_in(const Box& box) const;

    const steam::SteamContext& steam_;
    WorldScene& scene_;
    const PathBakeSettings settings_;
    LatestValue<ListenerPose> listener_;

    mutable std::mutex mutex_;  // current_, stats_, versions_
    std::shared_ptr<const PathBatch> current_;
    std::vector<ChunkKey> keys_;         // the chunks the current batch was baked from
    std::vector<uint64_t> versions_;     // and their versions then
    double changed_at_ = -1.0;           // when a change was first seen (caller's clock); -1 none
    PathBakeStats stats_;
    uint64_t next_id_ = 1;

    // Read by set_listener on the game's thread while a bake runs on ours.
    std::atomic<bool> baking_{false};
    std::atomic<bool> abandoned_{false};
    std::atomic<float> baking_centre_[3] = {};

    std::mutex thread_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;
};

}  // namespace vsa::world
