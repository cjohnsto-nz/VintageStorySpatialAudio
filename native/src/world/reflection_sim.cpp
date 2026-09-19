#include "world/reflection_sim.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/thread_stats.hpp"
#include "steam/steam_context.hpp"
#include "world/direct_sim.hpp"
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

// Of the convolved part, the fraction at its end that cross-fades into the parametric tail.
constexpr float kOverlap = 0.25f;

// Steam Audio averages its runs for as long as the listener and a source stay exactly where they
// are, and starts afresh the moment either moves at all: the listener and the sources are
// simulated from where they were until they have moved this far, so the noise of single runs
// settles while one stands and works.
constexpr double kHoldMetres = 0.5;

/// Moves `held` to `now` if it is further away than kHoldMetres (or `fresh`).
void hold(double held[3], const double now[3], bool fresh) {
    const double dx = now[0] - held[0];
    const double dy = now[1] - held[1];
    const double dz = now[2] - held[2];
    if (fresh || dx * dx + dy * dy + dz * dz > kHoldMetres * kHoldMetres) {
        std::copy_n(now, 3, held);
    }
}

}  // namespace

ReflectionSimulator::ReflectionSimulator(const steam::SteamContext& steam, WorldScene& scene,
                                         ReflectionChannel& channel, const ReflectionSettings& settings,
                                         uint32_t sample_rate, uint32_t frame_size)
    : steam_(steam),
      scene_(scene),
      channel_(channel),
      settings_(settings),
      sample_rate_(sample_rate),
      frame_size_(frame_size),
      ir_channels_((settings.order + 1) * (settings.order + 1)) {
    const auto started = std::chrono::steady_clock::now();
    IPLSimulationSettings sim{};
    sim.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    sim.sceneType = steam_.scene_type();
    sim.reflectionType = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    sim.maxNumRays = static_cast<IPLint32>(settings_.rays);
    sim.numDiffuseSamples = 32;
    sim.maxDuration = settings_.duration;
    sim.maxOrder = static_cast<IPLint32>(settings_.order);
    sim.maxNumSources = static_cast<IPLint32>(channel_.size());
    sim.numThreads = static_cast<IPLint32>(settings_.threads);
    sim.samplingRate = static_cast<IPLint32>(sample_rate_);
    sim.frameSize = static_cast<IPLint32>(frame_size_);
    check(iplSimulatorCreate(steam_.context(), &sim, simulator_.out()), "iplSimulatorCreate (reflections)");
    scene_.attach(simulator_.get());

    // Every slot's source exists (and is added) for the simulator's whole life: the render
    // thread's effects read their impulse responses; a slot not in use is merely disabled.
    sources_.resize(channel_.size());
    try {
        for (Source& source : sources_) {
            IPLSourceSettings source_settings{};
            source_settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            check(iplSourceCreate(simulator_.get(), &source_settings, source.handle.out()), "iplSourceCreate");
            iplSourceAdd(source.handle.get(), simulator_.get());
            IPLSimulationOutputs outputs{};
            iplSourceGetOutputs(source.handle.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
            source.ir = outputs.reflections.ir;
        }
        std::shared_lock lock(scene_.scene_lock());
        iplSimulatorCommit(simulator_.get());
    } catch (...) {
        for (Source& source : sources_) {
            if (source.handle) {
                iplSourceRemove(source.handle.get(), simulator_.get());
            }
        }
        {
            std::shared_lock lock(scene_.scene_lock());
            iplSimulatorCommit(simulator_.get());
        }
        sources_.clear();
        scene_.detach(simulator_.get());
        throw;
    }
    debug_.reserve(sources_.size());
    stats_.slots = static_cast<uint32_t>(sources_.size()) - 1;
    stats_.settings = settings_;
    Log::writef(VSA_LOG_INFO,
                "reflections: %u voice slots + listener reverb, %u rays x %u bounces, %.1f s order %u, %u Hz, "
                "%u threads, %.2f s convolved (%.0f ms)",
                stats_.slots, settings_.rays, settings_.bounces, static_cast<double>(settings_.duration), settings_.order,
                settings_.rate_hz, settings_.threads, static_cast<double>(settings_.transition), since_ms(started));
}

ReflectionSimulator::~ReflectionSimulator() {
    set_threaded(false);
    // Remove + Commit before Release.
    for (Source& source : sources_) {
        iplSourceRemove(source.handle.get(), simulator_.get());
    }
    {
        std::shared_lock lock(scene_.scene_lock());
        iplSimulatorCommit(simulator_.get());
    }
    sources_.clear();
    scene_.detach(simulator_.get());
}

void ReflectionSimulator::set_threaded(bool threaded) {
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

void ReflectionSimulator::thread_main() {
    ThreadScope scope("reflection simulation");
    const auto period = std::chrono::microseconds(1'000'000 / std::max(1u, settings_.rate_hz));
    constexpr auto kPoll = std::chrono::milliseconds(4);  // how soon a new place's first run starts
    auto next = std::chrono::steady_clock::now();
    std::unique_lock lock(thread_mutex_);
    while (!stop_) {
        const auto now = std::chrono::steady_clock::now();
        const bool urgent = has_new();
        if (!urgent && now < next) {
            wake_.wait_until(lock, std::min(next, now + kPoll), [this] { return stop_; });
            continue;
        }
        lock.unlock();
        try {
            tick(urgent);
        } catch (const std::exception& e) {
            Log::writef(VSA_LOG_ERROR, "reflection simulation: %s", e.what());
        }
        lock.lock();
        const auto done = std::chrono::steady_clock::now();
        // At the rate asked for, but resting at least as long as each run took: whatever the
        // quality settings, the simulation keeps its threads busy at most half the time. An
        // urgent run (new places only) leaves the regular cadence as it was.
        next = urgent ? std::max(next, done + (done - now)) : std::max(next + period, done + (done - now));
    }
}

void ReflectionSimulator::offline_tick(double seconds) {
    if (has_new()) {
        tick(true);
    }
    if (!have_ticked_ || seconds >= next_offline_) {
        tick(false);
        have_ticked_ = true;
        next_offline_ = seconds + 1.0 / std::max(1u, settings_.rate_hz);
    }
}

bool ReflectionSimulator::has_new() const noexcept {
    for (uint32_t i = 1; i < sources_.size(); ++i) {
        const uint32_t generation = channel_.input(i).generation.load(std::memory_order_acquire);
        if (generation != 0 && sources_[i].simulated != generation) {
            return true;
        }
    }
    return false;
}

void ReflectionSimulator::tick(bool urgent) {
    const auto started = std::chrono::steady_clock::now();
    const ListenerPose pose = listener_.read();
    const std::shared_ptr<const VoxelView> view = scene_.voxel_view();
    const int32_t* origin = view->origin();
    double listener_world[3];
    DirectSimulator::listen_from(*view, pose, listener_world);
    hold(held_listener_, listener_world, !listener_held_);
    listener_held_ = true;
    std::copy_n(held_listener_, 3, listener_world);
    float listener_scene[3];
    for (int k = 0; k < 3; ++k) {
        listener_scene[k] = static_cast<float>(listener_world[k] - origin[k]);
    }

    // Which slots run this time. Urgent: only the new places (their first result). Otherwise
    // the listener's, then new places, then the rest round-robin.
    const auto count = static_cast<uint32_t>(sources_.size());
    std::vector<bool> run(count, false);
    run[0] = !urgent;
    uint32_t budget = per_run();
    uint32_t active = 0;
    for (uint32_t i = 1; i < count; ++i) {
        Source& source = sources_[i];
        source.generation = channel_.input(i).generation.load(std::memory_order_acquire);
        active += source.generation != 0 ? 1u : 0u;
        if (source.generation != 0 && source.simulated != source.generation && budget > 0) {
            run[i] = true;
            --budget;
        }
    }
    const uint32_t start = cursor_;
    for (uint32_t n = 0; !urgent && n + 1 < count && budget > 0; ++n) {
        const uint32_t i = 1 + (start - 1 + n) % (count - 1);
        if (sources_[i].generation != 0 && !run[i]) {
            run[i] = true;
            --budget;
            cursor_ = 1 + i % (count - 1);
        }
    }

    std::vector<ReflectionSlotDebug> debug;
    debug.reserve(sources_.size());
    for (uint32_t i = 0; i < count; ++i) {
        Source& source = sources_[i];
        if (i == 0) {
            source.generation = ReflectionChannel::kListenerGeneration;
        }
        const uint32_t generation = source.generation;
        IPLSimulationInputs inputs{};
        if (!run[i]) {
            if (source.enabled) {
                inputs.flags = static_cast<IPLSimulationFlags>(0);  // disables the source's reflections
                iplSourceSetInputs(source.handle.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
                source.enabled = false;
            }
            if (generation != 0 && source.simulated == generation) {
                debug.push_back(source.last);  // waiting its turn: its latest results stand
            }
            continue;
        }

        ReflectionSlotDebug d;
        d.slot = i;
        double world[3];
        if (i == 0) {
            std::copy_n(listener_world, 3, world);
        } else {
            const ReflectionInput& in = channel_.input(i);
            d.voice = in.voice.load(std::memory_order_relaxed);
            world[0] = static_cast<double>(in.x.load(std::memory_order_relaxed)) + origin[0];
            world[1] = static_cast<double>(in.y.load(std::memory_order_relaxed)) + origin[1];
            world[2] = static_cast<double>(in.z.load(std::memory_order_relaxed)) + origin[2];
            // Out of the block the sound sits in, as for the direct path: rays leaving from inside
            // a solid block would never get out.
            view->escape(world, listener_world, 2, 0.25);
            view->clearance(world, 0.25);
            hold(source.held, world, source.held_generation != generation);
            source.held_generation = generation;
            std::copy_n(source.held, 3, world);
        }
        for (int k = 0; k < 3; ++k) {
            d.position[k] = static_cast<float>(world[k] - origin[k]);
        }
        inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        inputs.source = pose_at(d.position);
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1.0f;
        inputs.hybridReverbTransitionTime = settings_.transition;
        inputs.hybridReverbOverlapPercent = kOverlap;
        inputs.baked = IPL_FALSE;
        iplSourceSetInputs(source.handle.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
        source.enabled = true;
        debug.push_back(d);
    }

    IPLSimulationSharedInputs shared{};
    shared.listener.right = {pose.right[0], pose.right[1], pose.right[2]};
    shared.listener.up = {pose.up[0], pose.up[1], pose.up[2]};
    shared.listener.ahead = {pose.forward[0], pose.forward[1], pose.forward[2]};
    shared.listener.origin = {listener_scene[0], listener_scene[1], listener_scene[2]};
    shared.numRays = static_cast<IPLint32>(settings_.rays);
    shared.numBounces = static_cast<IPLint32>(settings_.bounces);
    shared.duration = settings_.duration;
    shared.order = static_cast<IPLint32>(settings_.order);
    shared.irradianceMinDistance = settings_.irradiance_min_distance;
    iplSimulatorSetSharedInputs(simulator_.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);

    const auto simulate_start = std::chrono::steady_clock::now();
    {
        std::shared_lock lock(scene_.scene_lock());
        iplSimulatorRunReflections(simulator_.get());
    }
    const double simulate_ms = since_ms(simulate_start);

    for (uint32_t i = 0; i < sources_.size(); ++i) {
        Source& source = sources_[i];
        ReflectionOutput& out = channel_.output(i);
        if (source.enabled) {
            IPLSimulationOutputs outputs{};
            iplSourceGetOutputs(source.handle.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
            ReflectionSlotDebug& d = *std::find_if(debug.begin(), debug.end(),
                                                   [i](const ReflectionSlotDebug& e) { return e.slot == i; });
            for (int b = 0; b < 3; ++b) {
                d.reverb_times[b] = outputs.reflections.reverbTimes[b];
                d.eq[b] = outputs.reflections.eq[b];
                out.reverb_times[b].store(d.reverb_times[b], std::memory_order_relaxed);
                out.eq[b].store(d.eq[b], std::memory_order_relaxed);
            }
            d.delay = outputs.reflections.delay;
            out.delay.store(d.delay, std::memory_order_relaxed);
            out.generation.store(source.generation, std::memory_order_release);
            source.simulated = source.generation;
            source.last = d;
        }
    }

    std::lock_guard lock(debug_mutex_);
    debug_.swap(debug);
    const double ms = since_ms(started);
    stats_.active = active;
    ++stats_.ticks;
    stats_.last_tick_ms = ms;
    stats_.max_tick_ms = std::max(stats_.max_tick_ms, ms);
    stats_.simulate_ms = simulate_ms;
    if (sources_[0].simulated != 0) {
        std::copy_n(sources_[0].last.reverb_times, 3, stats_.listener_reverb_times);
    }
    std::copy_n(listener_scene, 3, stats_.listener);
}

ReflectionStats ReflectionSimulator::stats() const {
    std::lock_guard lock(debug_mutex_);
    return stats_;
}

std::vector<ReflectionSlotDebug> ReflectionSimulator::slots() const {
    std::lock_guard lock(debug_mutex_);
    return debug_;
}

}  // namespace vsa::world
