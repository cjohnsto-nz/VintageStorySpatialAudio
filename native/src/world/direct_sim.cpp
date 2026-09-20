#include "world/direct_sim.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/thread_stats.hpp"
#include "steam/steam_context.hpp"
#include "world/world_scene.hpp"

#include <algorithm>
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

}  // namespace

void DirectSimulator::listen_from(const VoxelView& view, const ListenerPose& pose, double world[3]) {
    const int32_t* origin = view.origin();
    for (int k = 0; k < 3; ++k) {
        world[k] = static_cast<double>(pose.position[k]) + origin[k];
    }
    // A camera inside a block (third person against a wall) listens from just outside it, towards
    // where it looks. Only what the head is really inside counts: walking through an open door,
    // the leaf beside the head must not throw the listener across the doorway.
    const double ahead[3] = {world[0] + static_cast<double>(pose.forward[0]) * 2.0,
                             world[1] + static_cast<double>(pose.forward[1]) * 2.0,
                             world[2] + static_cast<double>(pose.forward[2]) * 2.0};
    view.escape(world, ahead, 2, 1e-3, VoxelView::Escaping::Enclosures);
}

DirectSimulator::DirectSimulator(const steam::SteamContext& steam, WorldScene& scene, DirectChannel& channel,
                                 uint32_t occlusion_samples, uint32_t rate_hz)
    : steam_(steam),
      scene_(scene),
      channel_(channel),
      samples_(std::clamp(occlusion_samples, 1u, 256u)),
      rate_hz_(std::clamp(rate_hz, 1u, 120u)),
      sources_(channel.size()) {
    IPLSimulationSettings settings{};
    settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    settings.sceneType = steam_.scene_type();
    settings.maxNumOcclusionSamples = static_cast<IPLint32>(samples_);
    settings.samplingRate = 48000;
    settings.frameSize = 256;
    check(iplSimulatorCreate(steam_.context(), &settings, simulator_.out()), "iplSimulatorCreate");
    scene_.attach(simulator_.get());
    debug_.reserve(channel.size());
    active_.reserve(channel.size());
    stats_.rate_hz = rate_hz_;
    stats_.occlusion_samples = samples_;
}

DirectSimulator::~DirectSimulator() {
    set_threaded(false);
    // Remove + Commit before Release.
    bool any = false;
    for (Source& s : sources_) {
        if (s.added) {
            {
                std::shared_lock membership(scene_.scene_lock());  // the scene worker commits this simulator
                iplSourceRemove(s.handle.get(), simulator_.get());
            }
            s.added = false;
            any = true;
        }
    }
    if (any) {
        std::shared_lock lock(scene_.scene_lock());
        iplSimulatorCommit(simulator_.get());
    }
    sources_.clear();
    scene_.detach(simulator_.get());
}

void DirectSimulator::set_threaded(bool threaded) {
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

void DirectSimulator::thread_main() {
    ThreadScope scope("direct simulation");
    const auto period = std::chrono::microseconds(1'000'000 / rate_hz_);
    auto next = std::chrono::steady_clock::now();
    std::unique_lock lock(thread_mutex_);
    while (!stop_) {
        lock.unlock();
        try {
            tick();
        } catch (const std::exception& e) {
            Log::writef(VSA_LOG_ERROR, "direct simulation: %s", e.what());
        }
        lock.lock();
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;  // fell behind: do not try to catch up
        }
        wake_.wait_until(lock, next, [this] { return stop_; });
    }
}

void DirectSimulator::offline_tick(double seconds) {
    if (!have_ticked_ || seconds >= next_offline_) {
        tick();
        have_ticked_ = true;
        next_offline_ = seconds + 1.0 / rate_hz_;
    }
}

void DirectSimulator::tick() {
    const auto started = std::chrono::steady_clock::now();
    const ListenerPose pose = listener_.read();
    const std::shared_ptr<const VoxelView> view = scene_.voxel_view();
    const int32_t* origin = view->origin();
    double listener_world[3];
    listen_from(*view, pose, listener_world);
    float listener_scene[3];
    for (int k = 0; k < 3; ++k) {
        listener_scene[k] = static_cast<float>(listener_world[k] - origin[k]);
    }

    channel_.scene_has_chunks.store(view->chunk_count() > 0, std::memory_order_relaxed);
    std::vector<Active>& active = active_;
    active.clear();

    // Membership follows the effect sets in use; positions escape the block they sit in.
    bool membership_changed = false;
    for (uint32_t i = 0; i < channel_.size(); ++i) {
        DirectInput& in = channel_.input(i);
        Source& source = sources_[i];
        const uint32_t generation = in.generation.load(std::memory_order_acquire);
        if (generation == 0) {
            if (source.added) {
                {
                    std::shared_lock membership(scene_.scene_lock());  // the scene worker commits this simulator
                    iplSourceRemove(source.handle.get(), simulator_.get());
                }
                source.added = false;
                membership_changed = true;
            }
            continue;
        }
        if (!source.handle) {
            IPLSourceSettings settings{};
            settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
            check(iplSourceCreate(simulator_.get(), &settings, source.handle.out()), "iplSourceCreate");
        }
        if (!source.added) {
            {
                std::shared_lock membership(scene_.scene_lock());  // the scene worker commits this simulator
                iplSourceAdd(source.handle.get(), simulator_.get());
            }
            source.added = true;
            membership_changed = true;
        }
        source.generation = generation;

        Active a{};
        a.set = i;
        a.generation = generation;
        a.debug.voice = in.voice.load(std::memory_order_relaxed);
        a.debug.position[0] = in.x.load(std::memory_order_relaxed);
        a.debug.position[1] = in.y.load(std::memory_order_relaxed);
        a.debug.position[2] = in.z.load(std::memory_order_relaxed);
        for (int k = 0; k < 3; ++k) {
            a.world[k] = static_cast<double>(a.debug.position[k]) + origin[k];
        }
        // Out of the block the sound sits in (full or partial), and far enough off its face that
        // the occlusion volume clears it.
        constexpr double kClear = 0.25;
        a.debug.escaped = view->escape(a.world, listener_world, 2, kClear);
        float radius = std::max(0.05f, in.radius.load(std::memory_order_relaxed));
        if (a.debug.escaped) {
            radius = std::min(radius, static_cast<float>(kClear));
        }
        view->clearance(a.world, static_cast<double>(radius));
        for (int k = 0; k < 3; ++k) {
            a.debug.simulated_position[k] = static_cast<float>(a.world[k] - origin[k]);
        }

        IPLSimulationInputs inputs{};
        inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
        inputs.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
        inputs.source = pose_at(a.debug.simulated_position);
        inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
        inputs.occlusionRadius = radius;
        inputs.numOcclusionSamples = static_cast<IPLint32>(samples_);
        iplSourceSetInputs(source.handle.get(), IPL_SIMULATIONFLAGS_DIRECT, &inputs);
        active.push_back(a);
    }

    IPLSimulationSharedInputs shared{};
    shared.listener.right = {pose.right[0], pose.right[1], pose.right[2]};
    shared.listener.up = {pose.up[0], pose.up[1], pose.up[2]};
    shared.listener.ahead = {pose.forward[0], pose.forward[1], pose.forward[2]};
    shared.listener.origin = {listener_scene[0], listener_scene[1], listener_scene[2]};
    iplSimulatorSetSharedInputs(simulator_.get(), IPL_SIMULATIONFLAGS_DIRECT, &shared);

    const auto occlusion_start = std::chrono::steady_clock::now();
    if (!active.empty() || membership_changed) {
        // The world scene commits scene changes to the simulator under this lock; source
        // changes need a commit of their own.
        std::shared_lock lock(scene_.scene_lock());
        if (membership_changed) {
            iplSimulatorCommit(simulator_.get());
        }
        if (!active.empty()) {
            iplSimulatorRunDirect(simulator_.get());
        }
    }
    const double occlusion_ms = since_ms(occlusion_start);

    const auto transmission_start = std::chrono::steady_clock::now();
    for (Active& a : active) {
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(sources_[a.set].handle.get(), IPL_SIMULATIONFLAGS_DIRECT, &outputs);
        const TransmissionTrace trace = view->trace(a.world, listener_world);
        a.debug.occlusion = std::clamp(outputs.direct.occlusion, 0.0f, 1.0f);
        a.debug.solid_metres = trace.solid_metres;
        a.debug.crossings = trace.crossings;
        DirectOutput& out = channel_.output(a.set);
        out.occlusion.store(a.debug.occlusion, std::memory_order_relaxed);
        for (int b = 0; b < 3; ++b) {
            a.debug.transmission[b] = trace.blocked() ? trace.gain(b) : kEdgeTransmission[b];
            out.transmission[b].store(a.debug.transmission[b], std::memory_order_relaxed);
        }
        out.generation.store(a.generation, std::memory_order_release);
    }
    const double transmission_ms = since_ms(transmission_start);

    std::lock_guard lock(debug_mutex_);
    debug_.clear();
    for (const Active& a : active) {
        debug_.push_back(a.debug);
    }
    const double ms = since_ms(started);
    stats_.sources = static_cast<uint32_t>(active.size());
    ++stats_.ticks;
    stats_.last_tick_ms = ms;
    stats_.max_tick_ms = std::max(stats_.max_tick_ms, ms);
    stats_.occlusion_ms = occlusion_ms;
    stats_.transmission_ms = transmission_ms;
    std::copy_n(listener_scene, 3, stats_.listener);
    std::copy_n(origin, 3, stats_.origin);
}

SimulationStats DirectSimulator::stats() const {
    std::lock_guard lock(debug_mutex_);
    return stats_;
}

std::vector<SourceDebug> DirectSimulator::sources() const {
    std::lock_guard lock(debug_mutex_);
    return debug_;
}

}  // namespace vsa::world
