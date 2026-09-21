#pragma once

#include "audio/mixer.hpp"
#include "audio/reflections.hpp"
#include "audio/spatial.hpp"
#include "core/latest_value.hpp"
#include "audio/voice.hpp"
#include "backend/device.hpp"
#include "backend/spatial_output.hpp"
#include "core/rt_log.hpp"
#include "core/spsc_ring.hpp"
#include "dsp/resampler.hpp"
#include "steam/steam_context.hpp"
#include "world/direct_sim.hpp"
#include "world/path_baker.hpp"
#include "world/path_channel.hpp"
#include "world/path_sim.hpp"
#include "world/ray_paths.hpp"
#include "world/reflection_channel.hpp"
#include "world/reflection_sim.hpp"
#include "world/world_scene.hpp"
#include "vsaudio.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vsa {

class Asset;
class Stream;

/// Root object behind the opaque `vsa_engine` handle.
///
/// Threads:
///  - render: the device callback, or the caller of render_offline(). Runs the Mixer only.
///  - worker: one native thread, woken every ~2 ms. Refills streams, returns retired voice slots
///    (dropping asset references, freeing streams), moves render events into the event queue,
///    forwards real-time log records and reopens lost devices.
///  - API threads: any. Voice calls post commands through one SPSC ring whose producer side is
///    serialised by api_mutex_, a lock the render thread never takes.
class Engine {
public:
    explicit Engine(const vsa_engine_config& config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] vsa_engine_info info() const noexcept;
    [[nodiscard]] vsa_self_test_report run_self_test() const;

    // Assets.
    [[nodiscard]] Asset* create_asset(const vsa_asset_desc& desc) const;

    // Voices.
    [[nodiscard]] vsa_voice create_voice(const vsa_voice_desc& desc);
    void release_voice(vsa_voice voice);
    void start_voice(vsa_voice voice);
    void pause_voice(vsa_voice voice);
    void stop_voice(vsa_voice voice);
    void set_voice_gain(vsa_voice voice, float gain);
    void set_voice_pitch(vsa_voice voice, float pitch);
    void set_voice_looping(vsa_voice voice, bool looping);
    void seek_voice(vsa_voice voice, double seconds);
    void fade_voice(vsa_voice voice, float target, float seconds, uint32_t flags, uint64_t token);
    void set_voice_position(vsa_voice voice, uint32_t spatial, float x, float y, float z);
    void set_voice_lowpass(vsa_voice voice, float gain_hf);
    void set_voice_occlusion_floor(vsa_voice voice, float floor);
    void set_voice_high_pass(vsa_voice voice, float hz);
    /// Debugging: silences a voice whatever its gain and fades say; it then asks for no simulation.
    void set_voice_muted(vsa_voice voice, bool muted);
    /// Debugging: gains on the direct sound and on the paths (the reflections have their own).
    void set_route_gains(float direct, float path);
    [[nodiscard]] vsa_voice_status voice_status(vsa_voice voice) const;

    void set_bus_gain(uint32_t bus, float gain);
    void set_master_gain(float gain);
    void set_listener(const vsa_listener& listener);
    void set_render_mode(uint32_t mode);
    void set_reflection_gain(float gain);
    void set_reflection_mix(float early, float tail);

    // Output.
    [[nodiscard]] std::vector<vsa_device_info> enumerate_devices();
    void open_output(const vsa_output_desc& desc);
    void render_offline(float* out, uint32_t frames);

    [[nodiscard]] vsa_engine_stats stats();
    [[nodiscard]] uint32_t poll_events(vsa_event* out, uint32_t capacity);

    /// Validated creation parameters.
    struct Settings {
        uint32_t sample_rate = 48000;
        uint32_t block_frames = 256;
        uint32_t max_voices = 4096;
        vsa_resampler_quality resampler_quality = VSA_RESAMPLER_MEDIUM;
        uint32_t stream_threshold_ms = 20000;
        uint32_t max_real_voices = 256;
        uint32_t max_binaural_voices = 64;
        std::string hrtf_sofa_path;  // empty: Steam Audio's default HRTF
        uint32_t occlusion_samples = 16;
        uint32_t direct_rate_hz = 30;
        bool direct_simulation = true;
        bool reflections = true;
        world::ReflectionSettings reflection;
        bool pathing = true;
        world::PathBakeSettings path_bake;
        world::PathSimSettings path_sim;
    };
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

    /// Turns the sound inspector on (the render thread then works out each voice's breakdown).
    void set_inspect(bool on) noexcept {
        mixer_.set_inspect(on);
        if (path_sim_) {
            path_sim_->set_baked_only(on);  // so that the legs drawn are the legs heard
        }
    }
    [[nodiscard]] bool inspecting() const noexcept { return mixer_.inspecting(); }
    /// What each voice sounded like in the latest block and how it reached the listener,
    /// loudest first. Empty unless the inspector is on.
    [[nodiscard]] std::vector<vsa_audible_voice> audible() const;

    /// A config with every field resolved: what a zeroed config would actually run as, including
    /// the values that depend on the machine (the reflection threads). The settings file is
    /// written from this, so it holds real numbers rather than zeros (ADR 0018).
    [[nodiscard]] static vsa_engine_config default_config();

    /// The world as Steam Audio geometry (thread-safe).
    [[nodiscard]] world::WorldScene& scene() noexcept { return *scene_; }
    /// The direct simulation (thread-safe queries); null when disabled.
    [[nodiscard]] world::DirectSimulator* direct() noexcept { return direct_sim_.get(); }

    /// The reflection simulation's state, for debugging views. `enabled` false when disabled.
    struct ReflectionReport {
        bool enabled = false;
        world::ReflectionStats stats;
        uint32_t live = 0;
        uint32_t waiting = 0;
        uint32_t draining = 0;
        float mean_square = 0.0f;
        float gain = 1.0f;
    };
    [[nodiscard]] ReflectionReport reflection_report();
    /// The reflection simulation; null when disabled. Replaced when the output's rate changes
    /// (tests use it with a fixed output).
    [[nodiscard]] world::ReflectionSimulator* reflection_simulator() noexcept { return reflection_sim_.get(); }
    [[nodiscard]] std::vector<world::ReflectionSlotDebug> reflection_slots();

    /// The pathing simulation and its baker (thread-safe queries); null when disabled.
    [[nodiscard]] world::PathSimulator* paths() noexcept { return path_sim_.get(); }
    [[nodiscard]] world::PathBaker* path_baker() noexcept { return path_baker_.get(); }

private:
    enum class StateChange { None, Start, Pause, Stop };

    /// Validates `voice` and posts `command` for it under the API lock, updating the voice's
    /// requested state/position first so status queries reflect it immediately.
    void post_voice_command(vsa_voice voice, Command command, StateChange change,
                            std::optional<double> new_position = std::nullopt);
    void post_global_command(const Command& command);
    [[nodiscard]] VoiceSlot& checked_slot(vsa_voice voice) const;
    [[nodiscard]] static uint32_t reported_state(const VoiceSlot& slot) noexcept;

    void worker_main();
    void service_streams() noexcept;
    void drain_retired();
    void drain_events_locked();  // requires events_mutex_
    void check_device();
    /// Opens the requested device: through Windows Spatial Audio when wants_spatial_ and it is
    /// available, else directly. Sets output_kind_. Requires output_mutex_.
    void open_device_locked(const vsa_device_id* id, uint32_t channels);
    void close_device_locked() noexcept;  // requires output_mutex_
    void push_event_locked(const vsa_event& event);                        // requires events_mutex_

    /// Everything that depends on the output's rate and layout: the reflection simulator (rebuilt
    /// when the rate changes), the reflection renderer and the mixer. Nothing renders meanwhile.
    void prepare_output(uint32_t sample_rate, uint32_t channels, const Speaker* speakers);
    void set_simulations_threaded(bool threaded);

    static void render_callback(void* user, float* out, uint32_t frames) noexcept;
    static void prepare_callback(void* user, uint32_t sample_rate, uint32_t channels, const Speaker* speakers);
    static void offline_block_hook(void* user) noexcept;

    Settings settings_;
    std::unique_ptr<steam::SteamContext> steam_;
    dsp::ResamplerKernel kernel_;

    // Voices.
    std::unique_ptr<VoiceSlot[]> slots_;
    std::vector<uint32_t> free_slots_;      // api_mutex_
    uint32_t allocated_voices_ = 0;         // api_mutex_
    mutable std::mutex api_mutex_;

    // Rings between threads.
    SpscRing<Command> commands_;   // API (serialised) -> render
    SpscRing<vsa_event> events_;   // render -> event queue (consumer serialised by events_mutex_)
    SpscRing<uint32_t> retired_;   // render -> worker
    RtLog rt_log_;

    std::unique_ptr<world::WorldScene> scene_;
    std::unique_ptr<world::DirectChannel> direct_channel_;
    std::unique_ptr<world::DirectSimulator> direct_sim_;
    std::unique_ptr<world::PathChannel> path_channel_;
    std::unique_ptr<world::PathBaker> path_baker_;
    std::unique_ptr<world::PathSimulator> path_sim_;  // after the baker: it lets go of the batch first
    std::unique_ptr<world::ReflectionChannel> reflection_channel_;
    // Replaced (under api_mutex_, with nothing rendering) when the output's rate changes.
    std::unique_ptr<world::ReflectionSimulator> reflection_sim_;
    double offline_seconds_ = 0.0;  // offline output time, for the synchronous simulation
    SpatialRenderer spatial_;
    LatestValue<ListenerPose> listener_;
    ListenerPose last_pose_;  // api_mutex_: for a rebuilt reflection simulator
    float reflection_gain_ = 1.0f;  // api_mutex_
    ReflectionRenderer reflections_;
    Mixer mixer_;
    uint32_t stream_history_frames_;
    uint32_t stream_window_frames_;

    // Streams being refilled (worker, or the offline renderer).
    std::mutex streams_mutex_;
    std::vector<Stream*> streams_;

    // Events for the API.
    std::mutex events_mutex_;
    std::deque<vsa_event> event_queue_;
    uint64_t events_dropped_ = 0;

    // Output. output_mutex_ also serialises offline rendering against output changes.
    std::mutex output_mutex_;
    backend::DeviceOutput device_;
    backend::SpatialOutput spatial_output_;
    uint32_t output_kind_ = VSA_OUTPUT_NONE;  // what runs: NONE, DEVICE or SPATIAL
    bool wants_spatial_ = false;              // SPATIAL was requested (DEVICE may be its fallback)
    std::optional<vsa_device_id> device_id_;  // the requested device; nullopt = follow the default
    uint32_t device_channels_ = 0;
    bool reopen_pending_ = false;
    std::chrono::steady_clock::time_point next_reopen_{};

    std::atomic<bool> running_{true};
    std::thread worker_;
};

}  // namespace vsa
