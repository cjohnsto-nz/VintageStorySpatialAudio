#include "world/path_baker.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
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

void PathBaker::bake(const Box& box) {
    {
        std::lock_guard lock(mutex_);
        stats_.baking = true;
    }
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

    // Probes on every floor in the box. Steam Audio centres its "unit cube" on the transform's
    // translation (it spans -0.5..0.5), whatever the header says.
    IPLProbeGenerationParams generation{};
    generation.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    generation.spacing = settings_.spacing;
    generation.height = settings_.probe_height;
    for (int k = 0; k < 3; ++k) {
        generation.transform.elements[k][k] = static_cast<float>(box.max[k] - box.min[k]);
        generation.transform.elements[k][3] = static_cast<float>((box.min[k] + box.max[k]) / 2.0 - origin[k]);
    }
    generation.transform.elements[3][3] = 1.0f;
    steam::ProbeArray array;
    check(iplProbeArrayCreate(steam_.context(), array.out()), "iplProbeArrayCreate");
    iplProbeArrayGenerateProbes(array.get(), snapshot->scene(), &generation);
    batch->probes = static_cast<uint32_t>(std::max(0, iplProbeArrayGetNumProbes(array.get())));
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

    std::lock_guard lock(mutex_);
    if (stop_) {
        stats_.baking = false;
        return;  // cancelled: the data is incomplete
    }
    batch->id = next_id_++;
    current_ = batch;
    keys_ = keys;
    versions_ = std::move(versions);
    changed_at_ = -1.0;
    stats_.baking = false;
    stats_.dirty = false;
    ++stats_.bakes;
    stats_.last_bake_ms = batch->bake_ms;
    stats_.max_bake_ms = std::max(stats_.max_bake_ms, batch->bake_ms);
    stats_.probes = batch->probes;
    for (int k = 0; k < 3; ++k) {
        stats_.centre[k] = (box.min[k] + box.max[k]) / 2.0;
    }
    Log::writef(VSA_LOG_INFO, "pathing: baked %u probes over %u x %u x %u blocks from %zu chunks in %.0f ms", batch->probes,
                settings_.range, settings_.height, settings_.range, snapshot->chunk_count(), batch->bake_ms);
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
