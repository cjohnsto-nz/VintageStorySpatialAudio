#include "engine.hpp"

#include "audio/asset.hpp"
#include "audio/stream.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/thread_stats.hpp"
#include "steam/self_test.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace vsa {
namespace {

constexpr std::size_t kCommandCapacity = 16384;
constexpr std::size_t kEventRingCapacity = 4096;
constexpr std::size_t kEventQueueLimit = 65536;
constexpr auto kWorkerPeriod = std::chrono::milliseconds(2);
constexpr auto kReopenInterval = std::chrono::seconds(1);
constexpr float kMaxGain = 64.0f;
constexpr float kMinPitch = 0.05f;
constexpr float kMaxPitch = 8.0f;

// Steam Audio's HRTF exists at 44.1 and 48 kHz (and 24 kHz, too low to be useful).
bool supported_rate(uint32_t rate) noexcept { return rate == 44100 || rate == 48000; }

Engine::Settings validate(const vsa_engine_config& config) {
    Engine::Settings settings;
    const auto invalid = [](const std::string& message) { return Error(VSA_ERROR_INVALID_ARGUMENT, message); };
    if (config.sample_rate != 0) {
        if (!supported_rate(config.sample_rate)) {
            throw invalid("sample_rate " + std::to_string(config.sample_rate) + " is not 44100 or 48000");
        }
        settings.sample_rate = config.sample_rate;
    }
    if (config.block_frames != 0) {
        if (config.block_frames < 32 || config.block_frames > 4096) {
            throw invalid("block_frames " + std::to_string(config.block_frames) + " is outside 32..4096");
        }
        settings.block_frames = config.block_frames;
    }
    if (config.max_voices != 0) {
        if (config.max_voices > 65536) {
            throw invalid("max_voices " + std::to_string(config.max_voices) + " exceeds 65536");
        }
        settings.max_voices = config.max_voices;
    }
    switch (config.resampler_quality) {
        case VSA_RESAMPLER_DEFAULT: settings.resampler_quality = VSA_RESAMPLER_MEDIUM; break;
        case VSA_RESAMPLER_LOW: settings.resampler_quality = VSA_RESAMPLER_LOW; break;
        case VSA_RESAMPLER_MEDIUM: settings.resampler_quality = VSA_RESAMPLER_MEDIUM; break;
        case VSA_RESAMPLER_HIGH: settings.resampler_quality = VSA_RESAMPLER_HIGH; break;
        default: throw invalid("unknown resampler_quality " + std::to_string(config.resampler_quality));
    }
    if (config.stream_threshold_ms != 0) {
        settings.stream_threshold_ms = config.stream_threshold_ms;
    }
    if (config.max_real_voices != 0) {
        if (config.max_real_voices > 4096) {
            throw invalid("max_real_voices " + std::to_string(config.max_real_voices) + " exceeds 4096");
        }
        settings.max_real_voices = config.max_real_voices;
    }
    if (config.max_binaural_voices != 0) {
        settings.max_binaural_voices = config.max_binaural_voices;
    }
    settings.max_binaural_voices = std::min(settings.max_binaural_voices, settings.max_real_voices);
    if (config.reserved != 0) {
        throw invalid("reserved must be 0");
    }
    if (config.hrtf_sofa_path != nullptr) {
        settings.hrtf_sofa_path = config.hrtf_sofa_path;
    }
    settings.direct_simulation = (config.flags & VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION) == 0;
    if (config.occlusion_samples != 0) {
        if (config.occlusion_samples > 256) {
            throw invalid("occlusion_samples " + std::to_string(config.occlusion_samples) + " exceeds 256");
        }
        settings.occlusion_samples = config.occlusion_samples;
    }
    if (config.direct_rate_hz != 0) {
        if (config.direct_rate_hz > 120) {
            throw invalid("direct_rate_hz " + std::to_string(config.direct_rate_hz) + " exceeds 120");
        }
        settings.direct_rate_hz = config.direct_rate_hz;
    }

    settings.reflections = (config.flags & VSA_ENGINE_FLAG_NO_REFLECTIONS) == 0;
    world::ReflectionSettings& r = settings.reflection;
    const auto range = [&](uint32_t value, uint32_t lo, uint32_t hi, const char* name, uint32_t& out) {
        if (value == 0) {
            return;
        }
        if (value < lo || value > hi) {
            throw invalid(std::string(name) + " " + std::to_string(value) + " is outside " + std::to_string(lo) + ".." +
                          std::to_string(hi));
        }
        out = value;
    };
    const auto range_f = [&](float value, float lo, float hi, const char* name, float& out) {
        if (value == 0.0f) {
            return;
        }
        if (!std::isfinite(value) || value < lo || value > hi) {
            throw invalid(std::string(name) + " " + std::to_string(value) + " is outside " + std::to_string(lo) + ".." +
                          std::to_string(hi));
        }
        out = value;
    };
    range(config.reflection_sources, 1, 64, "reflection_sources", r.sources);
    range(config.reflection_rays, 256, 32768, "reflection_rays", r.rays);
    range(config.reflection_bounces, 1, 64, "reflection_bounces", r.bounces);
    range_f(config.reflection_duration, 0.25f, 4.0f, "reflection_duration", r.duration);
    range(config.reflection_order, 1, 3, "reflection_order", r.order);
    range(config.reflection_rate_hz, 1, 60, "reflection_rate_hz", r.rate_hz);
    r.threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    range(config.reflection_threads, 1, 32, "reflection_threads", r.threads);
    range_f(config.reflection_transition, 0.02f, 0.5f, "reflection_transition", r.transition);
    if (r.transition >= r.duration) {
        throw invalid("reflection_transition must be shorter than reflection_duration");
    }

    settings.pathing = (config.flags & VSA_ENGINE_FLAG_NO_PATHING) == 0;
    world::PathBakeSettings& b = settings.path_bake;
    range(config.pathing_range, 32, 256, "pathing_range", b.range);
    range(config.pathing_height, 16, 128, "pathing_height", b.height);
    range_f(config.pathing_probe_spacing, 1.0f, 8.0f, "pathing_probe_spacing", b.spacing);
    range(config.pathing_vis_samples, 1, 8, "pathing_vis_samples", b.vis_samples);
    range(config.pathing_rate_hz, 1, 60, "pathing_rate_hz", settings.path_sim.rate_hz);
    range(config.pathing_sources, 1, 256, "pathing_sources", settings.path_sim.max_sources);
    range(config.pathing_max_probes, 64, 65536, "pathing_max_probes", b.max_probes);
    settings.path_sim.max_sources = std::min(settings.path_sim.max_sources, settings.max_real_voices);
    b.vis_range = static_cast<float>(b.range) / 3.0f;
    b.path_range = static_cast<float>(b.range);
    return settings;
}

std::unique_ptr<steam::SteamContext> make_steam(const vsa_engine_config& config) {
    steam::SteamContext::Options options;
    options.ray_tracer = static_cast<vsa_ray_tracer>(config.ray_tracer);  // validated in api.cpp
    options.validation = (config.flags & VSA_ENGINE_FLAG_STEAM_AUDIO_VALIDATION) != 0;
    return std::make_unique<steam::SteamContext>(options);
}

bool finite3(float x, float y, float z) noexcept { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }

/// Normalises `v` in place; false if it has no usable length.
bool normalise(float* v) noexcept {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(length) || length < 1e-6f) {
        return false;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

void check_gain(float gain, const char* what) {
    if (!std::isfinite(gain) || gain < 0.0f || gain > kMaxGain) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, std::string(what) + " must be a finite value in 0..64");
    }
}

void check_pitch(float pitch) {
    if (!std::isfinite(pitch) || pitch < kMinPitch || pitch > kMaxPitch) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "pitch must be in 0.05..8");
    }
}

const char* quality_name(vsa_resampler_quality quality) noexcept {
    switch (quality) {
        case VSA_RESAMPLER_LOW: return "low";
        case VSA_RESAMPLER_HIGH: return "high";
        default: return "medium";
    }
}

vsa_event make_event(vsa_event_type type) noexcept {
    vsa_event event{};
    event.struct_size = sizeof event;
    event.type = static_cast<uint32_t>(type);
    return event;
}

}  // namespace

vsa_engine_config Engine::default_config() {
    vsa_engine_config config{};
    config.struct_size = sizeof config;
    config.abi_version = VSA_ABI_VERSION;
    const Settings s = validate(config);
    config.sample_rate = s.sample_rate;
    config.block_frames = s.block_frames;
    config.max_voices = s.max_voices;
    config.resampler_quality = static_cast<uint32_t>(s.resampler_quality);
    config.stream_threshold_ms = s.stream_threshold_ms;
    config.max_real_voices = s.max_real_voices;
    config.max_binaural_voices = s.max_binaural_voices;
    config.occlusion_samples = s.occlusion_samples;
    config.direct_rate_hz = s.direct_rate_hz;
    config.reflection_sources = s.reflection.sources;
    config.reflection_rays = s.reflection.rays;
    config.reflection_bounces = s.reflection.bounces;
    config.reflection_duration = s.reflection.duration;
    config.reflection_order = s.reflection.order;
    config.reflection_rate_hz = s.reflection.rate_hz;
    config.reflection_threads = s.reflection.threads;
    config.reflection_transition = s.reflection.transition;
    config.pathing_range = s.path_bake.range;
    config.pathing_height = s.path_bake.height;
    config.pathing_probe_spacing = s.path_bake.spacing;
    config.pathing_vis_samples = s.path_bake.vis_samples;
    config.pathing_max_probes = s.path_bake.max_probes;
    config.pathing_rate_hz = s.path_sim.rate_hz;
    config.pathing_sources = s.path_sim.max_sources;
    return config;
}

Engine::Engine(const vsa_engine_config& config)
    : settings_(validate(config)),
      steam_(make_steam(config)),
      kernel_(settings_.resampler_quality),
      slots_(std::make_unique<VoiceSlot[]>(settings_.max_voices)),
      commands_(kCommandCapacity),
      events_(kEventRingCapacity),
      retired_(settings_.max_voices),
      scene_(std::make_unique<world::WorldScene>(*steam_)),
      direct_channel_(settings_.direct_simulation ? std::make_unique<world::DirectChannel>(settings_.max_real_voices)
                                                  : nullptr),
      direct_sim_(settings_.direct_simulation
                      ? std::make_unique<world::DirectSimulator>(*steam_, *scene_, *direct_channel_,
                                                                 settings_.occlusion_samples, settings_.direct_rate_hz)
                      : nullptr),
      path_channel_(settings_.pathing ? std::make_unique<world::PathChannel>(settings_.max_real_voices) : nullptr),
      path_baker_(settings_.pathing ? std::make_unique<world::PathBaker>(*steam_, *scene_, settings_.path_bake) : nullptr),
      path_sim_(settings_.pathing ? std::make_unique<world::PathSimulator>(*steam_, *scene_, *path_baker_, *path_channel_,
                                                                           settings_.path_sim, settings_.path_bake)
                                  : nullptr),
      reflection_channel_(settings_.reflections ? std::make_unique<world::ReflectionChannel>(settings_.reflection.sources + 1)
                                                : nullptr),
      spatial_(*steam_, settings_.max_real_voices, settings_.hrtf_sofa_path),
      reflections_(*steam_, reflection_channel_.get(), settings_.block_frames),
      mixer_(kernel_, slots_.get(), settings_.max_voices, commands_, events_, retired_, rt_log_, spatial_, listener_,
             settings_.block_frames, settings_.max_binaural_voices, direct_channel_.get(), &reflections_,
             path_channel_.get()),
      stream_history_frames_(static_cast<uint32_t>(kernel_.max_taps_per_side())),
      stream_window_frames_(kernel_.max_span_frames(settings_.block_frames)) {
    free_slots_.reserve(settings_.max_voices);
    for (uint32_t i = settings_.max_voices; i > 0; --i) {
        free_slots_.push_back(i - 1);  // hand out low slots first
    }
    prepare_output(settings_.sample_rate, 2, nullptr);

    worker_ = std::thread(&Engine::worker_main, this);
    Log::writef(VSA_LOG_INFO,
                "engine: %u-frame blocks, %u voice slots (%u real positional, %u binaural), %s-quality resampler",
                settings_.block_frames, settings_.max_voices, settings_.max_real_voices, settings_.max_binaural_voices,
                quality_name(settings_.resampler_quality));
}

Engine::~Engine() {
    {
        std::lock_guard lock(output_mutex_);
        close_device_locked();
        output_kind_ = VSA_OUTPUT_NONE;
    }
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    // Nothing renders any more: drop what live and retired voices still hold.
    for (uint32_t i = 0; i < settings_.max_voices; ++i) {
        VoiceSlot& slot = slots_[i];
        delete slot.stream;
        slot.stream = nullptr;
        if (slot.asset != nullptr) {
            slot.asset->release();
            slot.asset = nullptr;
        }
    }
    rt_log_.drain();
    // The reflection effects before the simulator whose impulse responses they read.
    reflections_.prepare(settings_.sample_rate, 2, nullptr);
    reflection_sim_.reset();
    // Steam Audio objects (spatial_, then steam_) are released by member destruction order.
    Log::write(VSA_LOG_INFO, "engine destroyed");
}

vsa_engine_info Engine::info() const noexcept {
    vsa_engine_info info{};
    info.struct_size = sizeof info;
    info.active_ray_tracer = static_cast<uint32_t>(steam_->active_ray_tracer());
    info.embree_available = steam_->embree_available() ? 1u : 0u;
    return info;
}

vsa_self_test_report Engine::run_self_test() const { return steam::run_self_test(*steam_); }

Asset* Engine::create_asset(const vsa_asset_desc& desc) const {
    return Asset::create(desc, settings_.stream_threshold_ms);
}

// ---------------------------------------------------------------------------------------------
// Voices

VoiceSlot& Engine::checked_slot(vsa_voice voice) const {
    const uint32_t index = handle_slot(voice);
    if (voice == 0 || index >= settings_.max_voices) {
        throw Error(VSA_ERROR_INVALID_HANDLE, "invalid voice handle");
    }
    VoiceSlot& slot = slots_[index];
    if (!slot.in_use.load(std::memory_order_acquire) ||
        slot.generation.load(std::memory_order_acquire) != handle_generation(voice)) {
        throw Error(VSA_ERROR_INVALID_HANDLE, "voice handle is stale (the voice was released)");
    }
    return slot;
}

uint32_t Engine::reported_state(const VoiceSlot& slot) noexcept {
    const uint32_t applied = slot.applied_seq.load(std::memory_order_acquire);
    const uint32_t issued = slot.cmd_seq.load(std::memory_order_acquire);
    return applied != issued ? slot.requested_state.load(std::memory_order_acquire)
                             : slot.render_state.load(std::memory_order_acquire);
}

vsa_voice Engine::create_voice(const vsa_voice_desc& desc) {
    auto* asset = reinterpret_cast<Asset*>(desc.asset);
    if (asset == nullptr) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "voice asset must not be null");
    }
    if (desc.bus >= VSA_BUS_COUNT) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown bus " + std::to_string(desc.bus));
    }
    check_gain(desc.gain, "gain");
    check_pitch(desc.pitch);
    if (desc.spatial > VSA_SPATIAL_LISTENER) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown spatial mode " + std::to_string(desc.spatial));
    }
    if (!finite3(desc.position[0], desc.position[1], desc.position[2])) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "voice position must be finite");
    }
    if (!std::isfinite(desc.min_distance) || desc.min_distance < 0.0f) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "min_distance must be finite and >= 0");
    }

    // Streams are opened and pre-filled here, on the caller's thread, so the voice can start
    // without an underrun; they are registered with the worker before the render thread sees them.
    std::unique_ptr<Stream> stream;
    if (asset->streamed()) {
        stream = std::make_unique<Stream>(*asset, stream_history_frames_, stream_window_frames_);
        stream->service();
        std::lock_guard lock(streams_mutex_);
        streams_.push_back(stream.get());
    }
    const auto unregister_stream = [&] {
        if (stream) {
            std::lock_guard lock(streams_mutex_);
            streams_.erase(std::remove(streams_.begin(), streams_.end(), stream.get()), streams_.end());
        }
    };

    std::unique_lock lock(api_mutex_);
    if (free_slots_.empty() || commands_.free_space() == 0) {
        const bool no_slots = free_slots_.empty();
        lock.unlock();
        unregister_stream();
        throw Error(VSA_ERROR_CAPACITY, no_slots ? "all " + std::to_string(settings_.max_voices) +
                                                       " voice slots are in use (see max_voices)"
                                                 : std::string("the command queue is full"));
    }
    const uint32_t index = free_slots_.back();
    free_slots_.pop_back();
    ++allocated_voices_;

    VoiceSlot& slot = slots_[index];
    slot.handle = make_handle(index, slot.generation.load(std::memory_order_relaxed));
    asset->add_ref();
    slot.asset = asset;
    slot.stream = stream.release();
    slot.bus = desc.bus;
    slot.initial_gain = desc.gain;
    slot.initial_pitch = desc.pitch;
    slot.initial_looping = desc.looping != 0;
    slot.initial_spatial = desc.spatial;
    slot.initial_position[0] = desc.position[0];
    slot.initial_position[1] = desc.position[1];
    slot.initial_position[2] = desc.position[2];
    slot.initial_min_distance = desc.min_distance > 0.0f ? desc.min_distance : 1.0f;
    slot.requested_state.store(VSA_VOICE_STOPPED, std::memory_order_relaxed);
    slot.cmd_seq.store(0, std::memory_order_relaxed);
    slot.render_state.store(VSA_VOICE_STOPPED, std::memory_order_relaxed);
    slot.applied_seq.store(0, std::memory_order_relaxed);
    slot.requested_position.store(0.0, std::memory_order_relaxed);
    slot.seek_seq.store(0, std::memory_order_relaxed);
    slot.seek_applied.store(0, std::memory_order_relaxed);
    slot.position.store(0.0, std::memory_order_relaxed);
    slot.in_use.store(true, std::memory_order_release);

    Command command{};
    command.op = Op::Activate;
    command.slot = index;
    commands_.try_push(command);  // space checked above; the lock keeps it
    return slot.handle;
}

void Engine::post_voice_command(vsa_voice voice, Command command, StateChange change,
                                std::optional<double> new_position) {
    std::lock_guard lock(api_mutex_);
    VoiceSlot& slot = checked_slot(voice);
    if (commands_.free_space() == 0) {
        throw Error(VSA_ERROR_CAPACITY, "the command queue is full");
    }
    command.slot = handle_slot(voice);

    if (change != StateChange::None) {
        const uint32_t current = reported_state(slot);
        uint32_t next = current;
        switch (change) {
            case StateChange::Start: next = VSA_VOICE_PLAYING; break;
            case StateChange::Pause: next = current == VSA_VOICE_PLAYING ? static_cast<uint32_t>(VSA_VOICE_PAUSED) : current; break;
            case StateChange::Stop: next = VSA_VOICE_STOPPED; break;
            case StateChange::None: break;
        }
        slot.requested_state.store(next, std::memory_order_relaxed);
        command.seq = slot.cmd_seq.load(std::memory_order_relaxed) + 1;
        slot.cmd_seq.store(command.seq, std::memory_order_release);
    }
    if (new_position) {
        const double duration = static_cast<double>(slot.asset->frames()) / slot.asset->sample_rate();
        command.position = std::clamp(*new_position, 0.0, duration);
        slot.requested_position.store(command.position, std::memory_order_relaxed);
        command.seek_seq = slot.seek_seq.load(std::memory_order_relaxed) + 1;
        slot.seek_seq.store(command.seek_seq, std::memory_order_release);
    }
    commands_.try_push(command);
}

void Engine::post_global_command(const Command& command) {
    std::lock_guard lock(api_mutex_);
    if (commands_.free_space() == 0) {
        throw Error(VSA_ERROR_CAPACITY, "the command queue is full");
    }
    commands_.try_push(command);
}

void Engine::release_voice(vsa_voice voice) {
    std::lock_guard lock(api_mutex_);
    VoiceSlot& slot = checked_slot(voice);
    if (commands_.free_space() == 0) {
        throw Error(VSA_ERROR_CAPACITY, "the command queue is full");
    }
    // The handle stops validating immediately; the slot returns to the free list once the render
    // thread has retired it and the worker has dropped its references.
    slot.in_use.store(false, std::memory_order_release);
    uint32_t generation = slot.generation.load(std::memory_order_relaxed) + 1;
    if (generation == 0) {
        generation = 1;  // handle 0 is never valid
    }
    slot.generation.store(generation, std::memory_order_release);

    Command command{};
    command.op = Op::Release;
    command.slot = handle_slot(voice);
    commands_.try_push(command);
}

void Engine::start_voice(vsa_voice voice) {
    Command command{};
    command.op = Op::Start;
    post_voice_command(voice, command, StateChange::Start);
}

void Engine::pause_voice(vsa_voice voice) {
    Command command{};
    command.op = Op::Pause;
    post_voice_command(voice, command, StateChange::Pause);
}

void Engine::stop_voice(vsa_voice voice) {
    Command command{};
    command.op = Op::Stop;
    post_voice_command(voice, command, StateChange::Stop, 0.0);
}

void Engine::set_voice_gain(vsa_voice voice, float gain) {
    check_gain(gain, "gain");
    Command command{};
    command.op = Op::SetGain;
    command.value = gain;
    post_voice_command(voice, command, StateChange::None);
}

void Engine::set_voice_pitch(vsa_voice voice, float pitch) {
    check_pitch(pitch);
    Command command{};
    command.op = Op::SetPitch;
    command.value = pitch;
    post_voice_command(voice, command, StateChange::None);
}

void Engine::set_voice_looping(vsa_voice voice, bool looping) {
    Command command{};
    command.op = Op::SetLooping;
    command.value = looping ? 1.0f : 0.0f;
    post_voice_command(voice, command, StateChange::None);
}

void Engine::seek_voice(vsa_voice voice, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "seek position must be a finite, non-negative number of seconds");
    }
    Command command{};
    command.op = Op::Seek;
    post_voice_command(voice, command, StateChange::None, seconds);
}

void Engine::fade_voice(vsa_voice voice, float target, float seconds, uint32_t flags, uint64_t token) {
    check_gain(target, "fade target");
    if (!std::isfinite(seconds) || seconds < 0.0f) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "fade duration must be a finite, non-negative number of seconds");
    }
    if ((flags & ~static_cast<uint32_t>(VSA_FADE_STOP_WHEN_DONE)) != 0) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown fade flags " + std::to_string(flags));
    }
    Command command{};
    command.op = Op::Fade;
    command.value = target;
    command.seconds = seconds;
    command.flags = flags;
    command.token = token;
    post_voice_command(voice, command, StateChange::None);
}

void Engine::set_voice_position(vsa_voice voice, uint32_t spatial, float x, float y, float z) {
    if (spatial > VSA_SPATIAL_LISTENER) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown spatial mode " + std::to_string(spatial));
    }
    if (!finite3(x, y, z)) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "voice position must be finite");
    }
    Command command{};
    command.op = Op::SetPosition;
    command.flags = spatial;
    command.vec[0] = x;
    command.vec[1] = y;
    command.vec[2] = z;
    post_voice_command(voice, command, StateChange::None);
}

void Engine::set_voice_lowpass(vsa_voice voice, float gain_hf) {
    if (!std::isfinite(gain_hf) || gain_hf < 0.0f || gain_hf > 1.0f) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "low-pass gain_hf must be in 0..1");
    }
    Command command{};
    command.op = Op::SetLowpass;
    command.value = gain_hf;
    post_voice_command(voice, command, StateChange::None);
}

vsa_voice_status Engine::voice_status(vsa_voice voice) const {
    const VoiceSlot& slot = checked_slot(voice);
    vsa_voice_status status{};
    status.struct_size = sizeof status;
    status.state = reported_state(slot);
    const uint32_t applied = slot.seek_applied.load(std::memory_order_acquire);
    const uint32_t issued = slot.seek_seq.load(std::memory_order_acquire);
    status.position_seconds = applied != issued ? slot.requested_position.load(std::memory_order_acquire)
                                                : slot.position.load(std::memory_order_acquire);
    return status;
}

void Engine::set_bus_gain(uint32_t bus, float gain) {
    if (bus >= VSA_BUS_COUNT) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown bus " + std::to_string(bus));
    }
    check_gain(gain, "bus gain");
    Command command{};
    command.op = Op::SetBusGain;
    command.slot = bus;
    command.value = gain;
    post_global_command(command);
}

void Engine::set_listener(const vsa_listener& listener) {
    ListenerPose pose;
    for (std::size_t i = 0; i < 3; ++i) {
        pose.position[i] = listener.position[i];
        pose.forward[i] = listener.forward[i];
        pose.up[i] = listener.up[i];
        pose.render_offset[i] = listener.render_offset[i];
    }
    if (!finite3(pose.render_offset[0], pose.render_offset[1], pose.render_offset[2])) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "listener render_offset must be finite");
    }
    if (!finite3(pose.position[0], pose.position[1], pose.position[2]) || !normalise(pose.forward) || !normalise(pose.up)) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "listener position must be finite and forward/up non-zero");
    }
    // right = forward x up; then re-derive up so the basis is orthonormal even if the caller's
    // vectors were not quite perpendicular.
    const float* f = pose.forward;
    float* u = pose.up;
    float* r = pose.right;
    r[0] = f[1] * u[2] - f[2] * u[1];
    r[1] = f[2] * u[0] - f[0] * u[2];
    r[2] = f[0] * u[1] - f[1] * u[0];
    if (!normalise(r)) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "listener forward and up must not be parallel");
    }
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    std::lock_guard lock(api_mutex_);  // LatestValue has a single writer
    listener_.publish(pose);
    last_pose_ = pose;
    if (direct_sim_) {
        direct_sim_->set_listener(pose);
    }
    if (reflection_sim_) {
        reflection_sim_->set_listener(pose);
    }
    if (path_sim_) {
        path_sim_->set_listener(pose);
        path_baker_->set_listener(pose);
    }
}

void Engine::set_reflection_gain(float gain) {
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "reflection gain must be a finite value in 0..4");
    }
    Command command{};
    command.op = Op::SetReflectionGain;
    command.value = gain;
    {
        std::lock_guard lock(api_mutex_);
        reflection_gain_ = gain;
    }
    post_global_command(command);
}

void Engine::set_reflection_mix(float early, float tail) {
    if (!std::isfinite(early) || !std::isfinite(tail) || early < 0.0f || tail < 0.0f || early > 4.0f || tail > 4.0f) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "reflection early and tail gains must be finite values in 0..4");
    }
    Command command{};
    command.op = Op::SetReflectionMix;
    command.value = early;
    command.seconds = tail;
    post_global_command(command);
}

Engine::ReflectionReport Engine::reflection_report() {
    ReflectionReport report;
    std::lock_guard lock(api_mutex_);
    report.gain = reflection_gain_;
    if (!reflection_sim_) {
        return report;
    }
    report.enabled = true;
    report.stats = reflection_sim_->stats();
    ReflectionMeter& meter = reflections_.meter();
    report.live = meter.live_slots.load(std::memory_order_relaxed);
    report.waiting = meter.waiting_slots.load(std::memory_order_relaxed);
    report.draining = meter.draining_slots.load(std::memory_order_relaxed);
    report.mean_square = meter.mean_square.load(std::memory_order_relaxed);
    return report;
}

std::vector<world::ReflectionSlotDebug> Engine::reflection_slots() {
    std::lock_guard lock(api_mutex_);
    return reflection_sim_ ? reflection_sim_->slots() : std::vector<world::ReflectionSlotDebug>{};
}

void Engine::set_render_mode(uint32_t mode) {
    if (mode != VSA_RENDER_HEADPHONES && mode != VSA_RENDER_SPEAKERS) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown render mode " + std::to_string(mode));
    }
    Command command{};
    command.op = Op::SetRenderMode;
    command.flags = mode;
    post_global_command(command);
}

void Engine::set_master_gain(float gain) {
    check_gain(gain, "master gain");
    Command command{};
    command.op = Op::SetMasterGain;
    command.value = gain;
    post_global_command(command);
}

// ---------------------------------------------------------------------------------------------
// Output

std::vector<vsa_device_info> Engine::enumerate_devices() {
    std::lock_guard lock(output_mutex_);
    return device_.enumerate();
}

void Engine::render_callback(void* user, float* out, uint32_t frames) noexcept {
    static_cast<Engine*>(user)->mixer_.render(out, frames, nullptr, nullptr);
}

void Engine::prepare_callback(void* user, uint32_t sample_rate, uint32_t channels, const Speaker* speakers) {
    static_cast<Engine*>(user)->prepare_output(sample_rate, channels, speakers);
}

void Engine::prepare_output(uint32_t sample_rate, uint32_t channels, const Speaker* speakers) {
    if (settings_.reflections && (!reflection_sim_ || reflection_sim_->sample_rate() != sample_rate)) {
        reflections_.prepare(sample_rate, channels, nullptr);  // lets go of the old impulse responses
        std::unique_ptr<world::ReflectionSimulator> simulator;
        try {
            simulator = std::make_unique<world::ReflectionSimulator>(*steam_, *scene_, *reflection_channel_,
                                                                     settings_.reflection, sample_rate,
                                                                     settings_.block_frames);
        } catch (const Error& e) {
            Log::writef(VSA_LOG_ERROR, "reflections unavailable: %s", e.what());
        }
        std::lock_guard lock(api_mutex_);
        reflection_sim_ = std::move(simulator);
        if (reflection_sim_) {
            reflection_sim_->set_listener(last_pose_);
        }
    }
    reflections_.prepare(sample_rate, channels, reflection_sim_.get());
    mixer_.prepare(sample_rate, channels, speakers);
}

void Engine::set_simulations_threaded(bool threaded) {
    if (direct_sim_) {
        direct_sim_->set_threaded(threaded);
    }
    if (reflection_sim_) {
        reflection_sim_->set_threaded(threaded);
    }
    if (path_sim_) {
        path_baker_->set_threaded(threaded);
        path_sim_->set_threaded(threaded);
    }
}

void Engine::offline_block_hook(void* user) noexcept {
    auto* engine = static_cast<Engine*>(user);
    engine->service_streams();
    // Deterministic offline rendering: the simulation runs on the rendering thread, on the
    // output's own clock.
    try {
        if (engine->direct_sim_ && !engine->direct_sim_->threaded()) {
            engine->direct_sim_->offline_tick(engine->offline_seconds_);
        }
    } catch (const std::exception& e) {
        Log::writef(VSA_LOG_ERROR, "direct simulation: %s", e.what());
    }
    try {
        if (engine->reflection_sim_ && !engine->reflection_sim_->threaded()) {
            engine->reflection_sim_->offline_tick(engine->offline_seconds_);
        }
    } catch (const std::exception& e) {
        Log::writef(VSA_LOG_ERROR, "reflection simulation: %s", e.what());
    }
    try {
        if (engine->path_sim_ && !engine->path_sim_->threaded()) {
            engine->path_baker_->offline_tick(engine->offline_seconds_);
            engine->path_sim_->offline_tick(engine->offline_seconds_);
        }
    } catch (const std::exception& e) {
        Log::writef(VSA_LOG_ERROR, "pathing: %s", e.what());
    }
    engine->offline_seconds_ += engine->mixer_.block_period_seconds();
}

void Engine::open_device_locked(const vsa_device_id* id, uint32_t channels) {
    close_device_locked();
    if (wants_spatial_) {
        try {
            spatial_output_.open(id, &render_callback, &prepare_callback, this);
            output_kind_ = VSA_OUTPUT_SPATIAL;
            set_simulations_threaded(true);
            return;
        } catch (const Error& e) {
            Log::writef(VSA_LOG_INFO, "spatial audio unavailable (%s); opening the device directly", e.what());
        }
    }
    device_.open(id, channels, &render_callback, &prepare_callback, this);
    output_kind_ = VSA_OUTPUT_DEVICE;
    set_simulations_threaded(true);
}

void Engine::close_device_locked() noexcept {
    device_.close();
    spatial_output_.close();
    set_simulations_threaded(false);
}

void Engine::open_output(const vsa_output_desc& desc) {
    if (desc.kind != VSA_OUTPUT_NONE && desc.kind != VSA_OUTPUT_DEVICE && desc.kind != VSA_OUTPUT_SPATIAL) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown output kind " + std::to_string(desc.kind));
    }
    if (desc.channels != 0 && !is_supported_layout(desc.channels)) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "channels must be 0, 2, 4, 6, 8 or 12");
    }
    const uint32_t rate = desc.sample_rate == 0 ? settings_.sample_rate : desc.sample_rate;
    if (desc.kind == VSA_OUTPUT_NONE && !supported_rate(rate)) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "sample_rate must be 44100 or 48000");
    }

    std::lock_guard lock(output_mutex_);
    close_device_locked();
    reopen_pending_ = false;
    wants_spatial_ = desc.kind == VSA_OUTPUT_SPATIAL;
    if (desc.kind == VSA_OUTPUT_NONE) {
        prepare_output(rate, desc.channels == 0 ? 2 : desc.channels, nullptr);
        output_kind_ = VSA_OUTPUT_NONE;
        device_id_.reset();
        return;
    }

    device_id_ = desc.device_id != nullptr ? std::optional<vsa_device_id>(*desc.device_id) : std::nullopt;
    device_channels_ = desc.channels;
    try {
        open_device_locked(device_id_ ? &*device_id_ : nullptr, device_channels_);
    } catch (...) {
        // Stay consistent: back to offline output at the configured rate.
        prepare_output(settings_.sample_rate, 2, nullptr);
        output_kind_ = VSA_OUTPUT_NONE;
        wants_spatial_ = false;
        device_id_.reset();
        throw;
    }
}

void Engine::render_offline(float* out, uint32_t frames) {
    if (out == nullptr && frames != 0) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "out must not be null");
    }
    std::lock_guard lock(output_mutex_);
    if (output_kind_ != VSA_OUTPUT_NONE) {
        throw Error(VSA_ERROR_INVALID_STATE, "offline rendering needs the NONE output (a device is open)");
    }
    mixer_.render(out, frames, &offline_block_hook, this);
}

// ---------------------------------------------------------------------------------------------
// Worker

void Engine::worker_main() {
    ThreadScope scope("engine worker");
    while (running_.load(std::memory_order_acquire)) {
        try {
            service_streams();
            drain_retired();
            {
                std::lock_guard lock(events_mutex_);
                drain_events_locked();
            }
            rt_log_.drain();
            check_device();
        } catch (const std::exception& e) {
            Log::writef(VSA_LOG_ERROR, "worker: %s", e.what());
        } catch (...) {
            Log::write(VSA_LOG_ERROR, "worker: unknown error");
        }
        std::this_thread::sleep_for(kWorkerPeriod);
    }
}

void Engine::service_streams() noexcept {
    std::lock_guard lock(streams_mutex_);
    for (Stream* stream : streams_) {
        stream->service();
    }
}

void Engine::drain_retired() {
    uint32_t index = 0;
    while (retired_.try_pop(index)) {
        VoiceSlot& slot = slots_[index];
        if (slot.stream != nullptr) {
            {
                std::lock_guard lock(streams_mutex_);
                streams_.erase(std::remove(streams_.begin(), streams_.end(), slot.stream), streams_.end());
            }
            delete slot.stream;
            slot.stream = nullptr;
        }
        if (slot.asset != nullptr) {
            slot.asset->release();
            slot.asset = nullptr;
        }
        std::lock_guard lock(api_mutex_);
        free_slots_.push_back(index);
        --allocated_voices_;
    }
}

void Engine::push_event_locked(const vsa_event& event) {
    if (event_queue_.size() >= kEventQueueLimit) {
        event_queue_.pop_front();
        ++events_dropped_;
    }
    event_queue_.push_back(event);
}

void Engine::drain_events_locked() {
    vsa_event event{};
    while (events_.try_pop(event)) {
        push_event_locked(event);
    }
}

void Engine::check_device() {
    std::lock_guard lock(output_mutex_);
    if (output_kind_ != VSA_OUTPUT_DEVICE && output_kind_ != VSA_OUTPUT_SPATIAL) {
        return;
    }
    const bool spatial = output_kind_ == VSA_OUTPUT_SPATIAL;
    if (!spatial) {
        ThreadRegistry::instance().announce(device_.render_thread_id(), "render (device callback)");
    }
    const auto now = std::chrono::steady_clock::now();
    if (spatial ? spatial_output_.take_rerouted() : device_.take_rerouted()) {
        // miniaudio follows the new default by itself; a spatial stream is tied to its endpoint
        // and is reopened on the new one.
        Log::write(VSA_LOG_INFO, "output followed the new default device");
        if (spatial && !reopen_pending_) {
            reopen_pending_ = true;
            next_reopen_ = now;
        }
        std::lock_guard events(events_mutex_);
        push_event_locked(make_event(VSA_EVENT_DEVICE_REROUTED));
    }
    if ((spatial ? spatial_output_.take_lost() : device_.take_lost()) && !reopen_pending_) {
        Log::write(VSA_LOG_WARNING, "output device lost; reopening");
        reopen_pending_ = true;
        next_reopen_ = now;
        std::lock_guard events(events_mutex_);
        push_event_locked(make_event(VSA_EVENT_DEVICE_LOST));
    }
    if (!reopen_pending_ || now < next_reopen_) {
        return;
    }

    bool reopened = false;
    try {
        open_device_locked(device_id_ ? &*device_id_ : nullptr, device_channels_);
        reopened = true;
    } catch (const Error& e) {
        if (device_id_) {
            Log::writef(VSA_LOG_WARNING, "reopening the selected device failed (%s); trying the default device",
                        e.what());
            try {
                open_device_locked(nullptr, device_channels_);
                reopened = true;
            } catch (const Error&) {
            }
        }
    }
    if (reopened) {
        reopen_pending_ = false;
        std::lock_guard events(events_mutex_);
        push_event_locked(make_event(VSA_EVENT_DEVICE_RESTORED));
    } else {
        next_reopen_ = now + kReopenInterval;
    }
}

// ---------------------------------------------------------------------------------------------
// Telemetry and events

vsa_engine_stats Engine::stats() {
    vsa_engine_stats stats{};
    stats.struct_size = sizeof stats;
    {
        std::lock_guard lock(output_mutex_);
        stats.output_kind = output_kind_;
        stats.sample_rate = mixer_.sample_rate();
        stats.channels = mixer_.channels();
        const bool spatial = output_kind_ == VSA_OUTPUT_SPATIAL && spatial_output_.is_open();
        if (spatial || (output_kind_ == VSA_OUTPUT_DEVICE && device_.is_open())) {
            const auto& format = spatial ? spatial_output_.format() : device_.format();
            stats.device_period_frames = format.period_frames;
            const std::size_t length = std::min(format.name.size(), sizeof stats.device_name - 1);
            std::copy_n(format.name.data(), length, stats.device_name);
            stats.device_name[length] = '\0';
        }
    }
    stats.block_frames = settings_.block_frames;
    stats.max_voices = settings_.max_voices;
    {
        std::lock_guard lock(api_mutex_);
        stats.allocated_voices = allocated_voices_;
    }

    MixerStats& m = mixer_.stats();
    stats.active_voices = m.active_voices.load(std::memory_order_relaxed);
    stats.real_voices = m.real_voices.load(std::memory_order_relaxed);
    stats.virtual_voices = m.virtual_voices.load(std::memory_order_relaxed);
    stats.blocks_rendered = m.blocks.load(std::memory_order_relaxed);
    stats.overloads = m.overloads.load(std::memory_order_relaxed);
    stats.stream_underruns = m.underruns.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(events_mutex_);
        stats.events_dropped = m.events_dropped.load(std::memory_order_relaxed) + events_dropped_;
    }
    const uint64_t sum = m.time_sum_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t count = m.time_count.exchange(0, std::memory_order_relaxed);
    const uint64_t max = m.time_max_ns.exchange(0, std::memory_order_relaxed);
    stats.render_time_avg_us = count == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(count) / 1000.0;
    stats.render_time_max_us = static_cast<double>(max) / 1000.0;
    stats.block_period_us = stats.sample_rate == 0 ? 0.0 : 1e6 * settings_.block_frames / stats.sample_rate;
    const float min_gain = m.min_limiter_gain.exchange(1.0f, std::memory_order_relaxed);
    stats.limiter_peak_reduction_db = min_gain >= 1.0f ? 0.0f : 20.0f * std::log10(std::max(min_gain, 1e-6f));
    return stats;
}

uint32_t Engine::poll_events(vsa_event* out, uint32_t capacity) {
    std::lock_guard lock(events_mutex_);
    drain_events_locked();
    const auto count = static_cast<uint32_t>(std::min<std::size_t>(capacity, event_queue_.size()));
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = event_queue_.front();
        event_queue_.pop_front();
    }
    return count;
}

}  // namespace vsa
