#include "world/path_sim.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/thread_stats.hpp"
#include "steam/steam_context.hpp"
#include "world/direct_sim.hpp"
#include "world/transmission.hpp"
#include "world/world_scene.hpp"

#include <algorithm>
#include <cmath>
#include <shared_mutex>
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

IPLCoordinateSpace3 pose_at(const float origin[3]) {
    IPLCoordinateSpace3 p{};
    p.right = {1.0f, 0.0f, 0.0f};
    p.up = {0.0f, 1.0f, 0.0f};
    p.ahead = {0.0f, 0.0f, -1.0f};
    p.origin = {origin[0], origin[1], origin[2]};
    return p;
}

constexpr std::size_t kMaxSegments = 4096;

}  // namespace

PathSimulator::PathSimulator(const steam::SteamContext& steam, WorldScene& scene, PathBaker& baker, PathChannel& channel,
                             const PathSimSettings& settings, const PathBakeSettings& bake)
    : steam_(steam), scene_(scene), baker_(baker), channel_(channel), settings_(settings), bake_(bake),
      sources_(channel.size()) {
    IPLSimulationSettings sim{};
    sim.flags = IPL_SIMULATIONFLAGS_PATHING;
    sim.sceneType = steam_.scene_type();
    sim.numVisSamples = static_cast<IPLint32>(std::max(1u, bake_.vis_samples));
    // The path coefficients are sized by maxOrder, and a run writes numCoeffs(pathingOrder) of
    // them regardless: with maxOrder 0 the three directional ones land past the buffer.
    sim.maxOrder = 1;
    sim.samplingRate = 48000;
    sim.frameSize = 256;
    check(iplSimulatorCreate(steam_.context(), &sim, simulator_.out()), "iplSimulatorCreate (pathing)");
    scene_.attach(simulator_.get());
    wanted_.reserve(channel.size());
    run_.assign(channel.size(), false);
    retired_.reserve(channel.size());
    segments_.reserve(kMaxSegments);
    debug_.reserve(kMaxSegments);
}

PathSimulator::~PathSimulator() {
    set_threaded(false);
    bool any = false;
    for (Source& s : sources_) {
        if (s.added) {
            iplSourceRemove(s.handle.get(), simulator_.get());
            s.added = false;
            any = true;
        }
    }
    if (batch_) {
        iplSimulatorRemoveProbeBatch(simulator_.get(), batch_->batch.get());
        any = true;
    }
    if (any) {
        std::shared_lock lock(scene_.scene_lock());
        iplSimulatorCommit(simulator_.get());
    }
    batch_.reset();
    sources_.clear();
    scene_.detach(simulator_.get());
}

void PathSimulator::set_threaded(bool threaded) {
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
        wake_.notify_all();
        thread_.join();
    }
}

void PathSimulator::thread_main() {
    ThreadScope scope("pathing simulation");
    const auto period = std::chrono::microseconds(1'000'000 / std::max(1u, settings_.rate_hz));
    auto next = std::chrono::steady_clock::now();
    std::unique_lock lock(thread_mutex_);
    while (!stop_) {
        lock.unlock();
        try {
            tick();
        } catch (const std::exception& e) {
            Log::writef(VSA_LOG_ERROR, "pathing simulation: %s", e.what());
        }
        lock.lock();
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;
        }
        wake_.wait_until(lock, next, [this] { return stop_; });
    }
}

void PathSimulator::offline_tick(double seconds) {
    if (!have_ticked_ || seconds >= next_offline_) {
        tick();
        have_ticked_ = true;
        next_offline_ = seconds + 1.0 / std::max(1u, settings_.rate_hz);
    }
}

void PathSimulator::on_segment(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* user) {
    auto& segments = *static_cast<std::vector<PathSegment>*>(user);
    if (segments.size() >= kMaxSegments) {
        return;
    }
    PathSegment s;
    s.from[0] = from.x;
    s.from[1] = from.y;
    s.from[2] = from.z;
    s.to[0] = to.x;
    s.to[1] = to.y;
    s.to[2] = to.z;
    s.occluded = occluded == IPL_TRUE;
    segments.push_back(s);
}

void PathSimulator::tick() {
    const auto started = std::chrono::steady_clock::now();
    const ListenerPose pose = listener_.read();
    const std::shared_ptr<const VoxelView> view = scene_.voxel_view();
    const int32_t* origin = view->origin();
    double listener_world[3];
    DirectSimulator::listen_from(*view, pose, listener_world);
    float listener_scene[3];
    for (int k = 0; k < 3; ++k) {
        listener_scene[k] = static_cast<float>(listener_world[k] - origin[k]);
    }

    // The baker's latest batch replaces the one in the simulator.
    bool membership_changed = false;
    std::shared_ptr<const PathBatch> old_batch;
    const std::shared_ptr<const PathBatch> latest = baker_.current();
    if (latest != batch_) {
        if (batch_) {
            iplSimulatorRemoveProbeBatch(simulator_.get(), batch_->batch.get());
            old_batch = std::move(batch_);
        }
        if (latest) {
            iplSimulatorAddProbeBatch(simulator_.get(), latest->batch.get());
        }
        batch_ = latest;
        membership_changed = true;
    }
    bool usable = batch_ != nullptr && batch_->probes > 0;
    for (int k = 0; k < 3 && usable; ++k) {
        usable = batch_->origin[k] == origin[k];  // baked under this origin (the baker follows)
    }

    // Who wants a path: the loudest max_sources of them.
    wanted_.clear();
    uint32_t wanted_count = 0;
    for (uint32_t i = 0; i < channel_.size(); ++i) {
        const PathInput& in = channel_.input(i);
        const uint32_t generation = in.generation.load(std::memory_order_acquire);
        if (generation != 0 && in.wanted.load(std::memory_order_relaxed) && usable) {
            wanted_.push_back({i, in.level.load(std::memory_order_relaxed)});
            ++wanted_count;
        }
    }
    if (wanted_.size() > settings_.max_sources) {
        std::nth_element(wanted_.begin(), wanted_.begin() + settings_.max_sources, wanted_.end(),
                         [](const Wanted& a, const Wanted& b) { return a.level > b.level; });
        wanted_.resize(settings_.max_sources);
    }
    std::vector<bool>& run = run_;
    std::fill(run.begin(), run.end(), false);
    for (const Wanted& w : wanted_) {
        run[w.set] = true;
    }
    for (uint32_t i = 0; i < channel_.size(); ++i) {
        Source& source = sources_[i];
        if (source.added) {
            iplSourceRemove(source.handle.get(), simulator_.get());
            source.added = false;
            retired_.push_back(std::move(source.handle));  // released once the removal is committed
            membership_changed = true;
        }
        if (!run[i]) {
            continue;
        }
        PathInput& in = channel_.input(i);
        // A fresh source every run. When Steam Audio finds no probe in reach of the source or the
        // listener (a sound beyond the box, or sealed in) it writes nothing and the source keeps
        // the coefficients of its last path: another sound taking over this set, or this one
        // moving out of reach, would play through that path. A new source's are zero.
        IPLSourceSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_PATHING;
        check(iplSourceCreate(simulator_.get(), &settings, source.handle.out()), "iplSourceCreate (pathing)");
        iplSourceAdd(source.handle.get(), simulator_.get());
        source.added = true;
        membership_changed = true;
        source.generation = in.generation.load(std::memory_order_acquire);
        double world[3] = {static_cast<double>(in.x.load(std::memory_order_relaxed)) + origin[0],
                           static_cast<double>(in.y.load(std::memory_order_relaxed)) + origin[1],
                           static_cast<double>(in.z.load(std::memory_order_relaxed)) + origin[2]};
        view->escape(world, listener_world, 2, 0.25);  // out of the block the sound sits in
        float scene_position[3];
        for (int k = 0; k < 3; ++k) {
            scene_position[k] = static_cast<float>(world[k] - origin[k]);
        }
        IPLSimulationInputs inputs{};
        inputs.flags = IPL_SIMULATIONFLAGS_PATHING;
        inputs.source = pose_at(scene_position);
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        inputs.pathingProbes = batch_->batch.get();
        inputs.visRadius = bake_.vis_radius;
        inputs.visThreshold = bake_.vis_threshold;
        inputs.visRange = bake_.vis_range;
        inputs.pathingOrder = 1;
        inputs.enableValidation = IPL_TRUE;
        inputs.findAlternatePaths = IPL_TRUE;
        iplSourceSetInputs(source.handle.get(), IPL_SIMULATIONFLAGS_PATHING, &inputs);
    }

    segments_.clear();
    IPLSimulationSharedInputs shared{};
    shared.listener.right = {pose.right[0], pose.right[1], pose.right[2]};
    shared.listener.up = {pose.up[0], pose.up[1], pose.up[2]};
    shared.listener.ahead = {pose.forward[0], pose.forward[1], pose.forward[2]};
    shared.listener.origin = {listener_scene[0], listener_scene[1], listener_scene[2]};
    shared.pathingVisCallback = &on_segment;
    shared.pathingUserData = &segments_;
    iplSimulatorSetSharedInputs(simulator_.get(), IPL_SIMULATIONFLAGS_PATHING, &shared);

    if (membership_changed || !wanted_.empty()) {
        std::shared_lock lock(scene_.scene_lock());
        if (membership_changed) {
            iplSimulatorCommit(simulator_.get());
        }
        if (!wanted_.empty()) {
            iplSimulatorRunPathing(simulator_.get());
        }
    }
    old_batch.reset();  // removed and committed: safe to release
    retired_.clear();   // likewise

    uint32_t found = 0;
    for (const Wanted& w : wanted_) {
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(sources_[w.set].handle.get(), IPL_SIMULATIONFLAGS_PATHING, &outputs);
        PathOutput& out = channel_.output(w.set);
        bool any = false;
        for (int c = 0; c < 4; ++c) {
            const float v = outputs.pathing.shCoeffs != nullptr ? outputs.pathing.shCoeffs[c] : 0.0f;
            out.sh[c].store(std::isfinite(v) ? v : 0.0f, std::memory_order_relaxed);
            any = any || std::abs(v) > 0.0f;
        }
        for (int b = 0; b < 3; ++b) {
            const float e = outputs.pathing.eqCoeffs[b];
            out.eq[b].store(std::isfinite(e) ? std::clamp(e, 0.0f, 1.0f) : 1.0f, std::memory_order_relaxed);
        }
        out.found.store(any, std::memory_order_relaxed);
        out.generation.store(sources_[w.set].generation, std::memory_order_release);
        found += any ? 1u : 0u;
    }

    std::lock_guard lock(debug_mutex_);
    debug_.swap(segments_);
    const double ms = since_ms(started);
    ++stats_.ticks;
    stats_.last_tick_ms = ms;
    stats_.max_tick_ms = std::max(stats_.max_tick_ms, ms);
    stats_.wanted = wanted_count;
    stats_.simulated = static_cast<uint32_t>(wanted_.size());
    stats_.found = found;
    stats_.batch_id = usable ? batch_->id : 0;
    std::copy_n(listener_scene, 3, stats_.listener);
}

PathSimStats PathSimulator::stats() const {
    std::lock_guard lock(debug_mutex_);
    return stats_;
}

std::vector<PathSegment> PathSimulator::segments() const {
    std::lock_guard lock(debug_mutex_);
    return debug_;
}

}  // namespace vsa::world
