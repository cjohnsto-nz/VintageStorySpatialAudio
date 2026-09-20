#include "world/path_baker.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/thread_stats.hpp"
#include "steam/steam_context.hpp"
#include "world/transmission.hpp"
#include "world/world_scene.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace vsa::world {
namespace {

void check(IPLerror error, const char* what) {
    if (error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string(what) + " failed: " + steam::error_name(error));
    }
}

double since_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Steam Audio 4.8.1's path baker calls its progress callback unconditionally: it must not be null.
void IPLCALL no_progress(IPLfloat32, void*) {}

constexpr auto kPoll = std::chrono::milliseconds(100);

}  // namespace

PathBaker::PathBaker(const steam::SteamContext& steam, WorldScene& scene, const PathBakeSettings& settings)
    : steam_(steam), scene_(scene), settings_(settings) {}

void PathBaker::set_listener(const ListenerPose& pose) noexcept {
    listener_.publish(pose);
    // A bake whose box the listener has left is worth nothing: its paths are for where they
    // were. Walking, a bake that is not abandoned is stale before it lands and the next one
    // starts that much later (ADR 0015). The same third of the box as `due` uses.
    if (!baking_.load(std::memory_order_acquire) || abandoned_.load(std::memory_order_relaxed)) {
        return;
    }
    const float size[3] = {static_cast<float>(settings_.range), static_cast<float>(settings_.height),
                           static_cast<float>(settings_.range)};
    for (int k = 0; k < 3; ++k) {
        const float centre = baking_centre_[k].load(std::memory_order_relaxed);
        if (std::abs(pose.position[k] - centre) > size[k] / 3.0f) {
            if (!abandoned_.exchange(true, std::memory_order_acq_rel)) {
                iplPathBakerCancelBake(steam_.context());
            }
            return;
        }
    }
}

PathBaker::~PathBaker() {
    if (thread_.joinable()) {
        {
            std::lock_guard lock(thread_mutex_);
            stop_ = true;
        }
        iplPathBakerCancelBake(steam_.context());
        wake_.notify_all();
        thread_.join();
    }
}

void PathBaker::set_threaded(bool threaded) {
    if (threaded == thread_.joinable()) {
        return;
    }
    if (threaded) {
        {
            std::lock_guard lock(thread_mutex_);
            stop_ = false;
        }
        thread_ = std::thread([this] { thread_main(); });
    } else {
        {
            std::lock_guard lock(thread_mutex_);
            stop_ = true;
        }
        iplPathBakerCancelBake(steam_.context());
        wake_.notify_all();
        thread_.join();
    }
}

void PathBaker::thread_main() {
    ThreadScope scope("path baker");
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(thread_mutex_);
    while (!stop_) {
        lock.unlock();
        Box box{};
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (due(now, box)) {
            try {
                bake(box);
            } catch (const std::exception& e) {
                Log::writef(VSA_LOG_ERROR, "pathing bake: %s", e.what());
            }
        }
        lock.lock();
        wake_.wait_for(lock, kPoll, [this] { return stop_; });
    }
}

void PathBaker::offline_tick(double seconds) {
    Box box{};
    if (due(seconds, box)) {
        bake(box);
    }
}

std::vector<ChunkKey> PathBaker::keys_in(const Box& box) const {
    // The box's chunks and a ring round them: probes near the edge see beyond it.
    std::vector<ChunkKey> keys;
    const auto floor_div = [](double v) { return static_cast<int32_t>(std::floor(v / kChunkSize)); };
    for (int32_t y = floor_div(box.min[1]) - 1; y <= floor_div(box.max[1] - 1e-6) + 1; ++y) {
        for (int32_t z = floor_div(box.min[2]) - 1; z <= floor_div(box.max[2] - 1e-6) + 1; ++z) {
            for (int32_t x = floor_div(box.min[0]) - 1; x <= floor_div(box.max[0] - 1e-6) + 1; ++x) {
                keys.push_back({x, y, z});
            }
        }
    }
    return keys;
}

bool PathBaker::due(double now, Box& box) {
    const ListenerPose pose = listener_.read();
    const std::shared_ptr<const VoxelView> view = scene_.voxel_view();
    const int32_t* origin = view->origin();
    double listener[3];
    for (int k = 0; k < 3; ++k) {
        listener[k] = static_cast<double>(pose.position[k]) + origin[k];
    }
    // The box: centred on the listener, snapped to 8 blocks so a small step does not move it.
    const double size[3] = {static_cast<double>(settings_.range), static_cast<double>(settings_.height),
                            static_cast<double>(settings_.range)};
    for (int k = 0; k < 3; ++k) {
        const double centre = std::round(listener[k] / 8.0) * 8.0;
        box.min[k] = centre - size[k] / 2.0;
        box.max[k] = centre + size[k] / 2.0;
    }

    std::lock_guard lock(mutex_);
    if (stats_.baking) {
        return false;
    }
    if (!current_) {
        stats_.dirty = view->chunk_count() > 0;
        return stats_.dirty;  // nothing to bake before the world is there
    }
    // The box round the listener has moved a third of its size from the current one, or the
    // origin moved: at once.
    bool left = false;
    for (int k = 0; k < 3 && !left; ++k) {
        const double centre = (current_->min[k] + current_->max[k]) / 2.0;
        left = std::abs((box.min[k] + box.max[k]) / 2.0 - centre) > size[k] / 3.0;
    }
    bool origin_moved = false;
    for (int k = 0; k < 3; ++k) {
        origin_moved = origin_moved || origin[k] != current_->origin[k];
    }
    if (left || origin_moved) {
        stats_.dirty = true;
        return true;
    }
    // Chunks changed: after a quiet spell, on the current box.
    const std::vector<uint64_t> versions = scene_.chunk_versions(keys_);
    if (versions != versions_) {
        if (changed_at_ < 0.0) {
            changed_at_ = now;
        }
        stats_.dirty = true;
        if (now - changed_at_ >= settings_.rebake_seconds) {
            for (int k = 0; k < 3; ++k) {
                box.min[k] = current_->min[k];
                box.max[k] = current_->max[k];
            }
            return true;
        }
        return false;
    }
    changed_at_ = -1.0;
    stats_.dirty = false;
    return false;
}

steam::ProbeArray PathBaker::generate(const Box& box, const IPLScene scene, const int32_t origin[3],
                                      uint32_t& probes, float& spacing) const {
    // Probes on every floor in the box. Steam Audio centres its "unit cube" on the transform's
    // translation (it spans -0.5..0.5), whatever the header says.
    IPLProbeGenerationParams generation{};
    generation.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    generation.height = settings_.probe_height;
    for (int k = 0; k < 3; ++k) {
        generation.transform.elements[k][k] = static_cast<float>(box.max[k] - box.min[k]);
        generation.transform.elements[k][3] = static_cast<float>((box.min[k] + box.max[k]) / 2.0 - origin[k]);
    }
    generation.transform.elements[3][3] = 1.0f;

    // Probes go as 1 / spacing^2, so a spacing scaled by sqrt(probes / budget) lands near the
    // budget; a little over, and at most a few tries, because the terrain decides the rest.
    const auto budget = static_cast<float>(std::max(1u, settings_.max_probes));
    spacing = settings_.spacing;
    steam::ProbeArray array;
    for (int attempt = 0; attempt < 4; ++attempt) {
        steam::ProbeArray candidate;
        check(iplProbeArrayCreate(steam_.context(), candidate.out()), "iplProbeArrayCreate");
        generation.spacing = spacing;
        iplProbeArrayGenerateProbes(candidate.get(), scene, &generation);
        probes = static_cast<uint32_t>(std::max(0, iplProbeArrayGetNumProbes(candidate.get())));
        array = std::move(candidate);
        if (probes <= settings_.max_probes) {
            break;
        }
        spacing *= 1.05f * std::sqrt(static_cast<float>(probes) / budget);
    }
    return array;
}

void PathBaker::bake(const Box& box) {
    const auto started = std::chrono::steady_clock::now();
    const std::vector<ChunkKey> keys = keys_in(box);
    std::vector<uint64_t> versions = scene_.chunk_versions(keys);
    const std::shared_ptr<const VoxelView> view = scene_.voxel_view();
    const int32_t* origin = view->origin();
    const std::unique_ptr<WorldScene::Snapshot> snapshot = scene_.snapshot(keys);

    auto batch = std::make_shared<PathBatch>();
    std::copy_n(origin, 3, batch->origin);
    std::copy_n(box.min, 3, batch->min);
    std::copy_n(box.max, 3, batch->max);

    // set_listener watches this box while the bake runs, and abandons it if the listener leaves.
    for (int k = 0; k < 3; ++k) {
        baking_centre_[k].store(static_cast<float>((box.min[k] + box.max[k]) / 2.0 - origin[k]), std::memory_order_relaxed);
    }
    abandoned_.store(false, std::memory_order_relaxed);
    baking_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        stats_.baking = true;
    }
    float spacing = settings_.spacing;
    const steam::ProbeArray array = generate(box, snapshot->scene(), origin, batch->probes, spacing);
    check(iplProbeBatchCreate(steam_.context(), batch->batch.out()), "iplProbeBatchCreate");
    iplProbeBatchAddProbeArray(batch->batch.get(), array.get());
    iplProbeBatchCommit(batch->batch.get());

    if (batch->probes > 0) {
        IPLPathBakeParams bake{};
        bake.scene = snapshot->scene();
        bake.probeBatch = batch->batch.get();
        bake.identifier.type = IPL_BAKEDDATATYPE_PATHING;
        bake.identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
        bake.numSamples = static_cast<IPLint32>(settings_.vis_samples);
        bake.radius = settings_.vis_radius;
        bake.threshold = settings_.vis_threshold;
        bake.visRange = settings_.vis_range;
        bake.pathRange = settings_.path_range;
        bake.numThreads = static_cast<IPLint32>(settings_.threads);
        iplPathBakerBake(steam_.context(), &bake, &no_progress, nullptr);
    }
    batch->bake_ms = since_ms(started);
    baking_.store(false, std::memory_order_release);

    std::lock_guard lock(mutex_);
    stats_.baking = false;
    if (stop_ || abandoned_.load(std::memory_order_acquire)) {
        // Cancelled: the data is incomplete, and where it is for is behind us. The next pass
        // starts a bake on the box the listener is in now.
        if (!stop_) {
            ++stats_.cancelled;
            Log::writef(VSA_LOG_DEBUG, "pathing: bake of %u probes abandoned after %.0f ms; the listener moved on",
                        batch->probes, batch->bake_ms);
        }
        return;
    }
    batch->id = next_id_++;
    current_ = batch;
    keys_ = keys;
    versions_ = std::move(versions);
    changed_at_ = -1.0;
    stats_.dirty = false;
    ++stats_.bakes;
    stats_.last_bake_ms = batch->bake_ms;
    stats_.max_bake_ms = std::max(stats_.max_bake_ms, batch->bake_ms);
    stats_.probes = batch->probes;
    stats_.spacing = spacing;
    for (int k = 0; k < 3; ++k) {
        stats_.centre[k] = (box.min[k] + box.max[k]) / 2.0;
    }
    Log::writef(VSA_LOG_INFO, "pathing: baked %u probes %.1f m apart over %u x %u x %u blocks from %zu chunks in %.0f ms",
                batch->probes, static_cast<double>(spacing), settings_.range, settings_.height, settings_.range,
                snapshot->chunk_count(), batch->bake_ms);
}

std::shared_ptr<const PathBatch> PathBaker::current() const {
    std::lock_guard lock(mutex_);
    return current_;
}

PathBakeStats PathBaker::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace vsa::world
