#pragma once

#include "audio/bed_panner.hpp"
#include "audio/channel_layout.hpp"
#include "audio/listener_pose.hpp"
#include "audio/reflections.hpp"
#include "audio/spatial.hpp"
#include "audio/voice.hpp"
#include "core/latest_value.hpp"
#include "core/rt_log.hpp"
#include "core/spsc_ring.hpp"
#include "decode/decoders.hpp"
#include "dsp/gain_ramp.hpp"
#include "dsp/limiter.hpp"
#include "dsp/resampler.hpp"
#include "vsaudio.h"
#include "world/direct_channel.hpp"
#include "world/path_channel.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <cstdint>
#include <vector>

namespace vsa {

class Asset;

/// Counters the render thread publishes; read (and partly reset) by API threads.
struct MixerStats {
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> overloads{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> events_dropped{0};
    std::atomic<uint64_t> time_sum_ns{0};
    std::atomic<uint64_t> time_count{0};
    std::atomic<uint64_t> time_max_ns{0};
    std::atomic<float> min_limiter_gain{1.0f};
    std::atomic<uint32_t> active_voices{0};
    std::atomic<uint32_t> real_voices{0};
    std::atomic<uint32_t> virtual_voices{0};
};

/// The render core. Everything here runs on the render thread (the device callback, or the
/// caller of an offline render) except the constructor and prepare(). It never allocates,
/// locks, logs directly or calls into managed code: it talks to the rest of the engine only
/// through the lock-free rings and the voice slots' atomics.
///
/// Per block: apply queued commands -> for each active voice, resample (+ stream window) ->
/// voice gain x declick envelope -> pan into its bus -> buses x bus gain -> master gain ->
/// true-peak limiter -> interleave to the output channel layout (front L/R; Phase 1 has no
/// spatialisation, other channels are silent).
class Mixer {
public:
    using BlockHook = void (*)(void* user) noexcept;

    Mixer(const dsp::ResamplerKernel& kernel, VoiceSlot* slots, uint32_t slot_count, SpscRing<Command>& commands,
          SpscRing<vsa_event>& events, SpscRing<uint32_t>& retired, RtLog& rt_log, SpatialRenderer& spatial,
          LatestValue<ListenerPose>& listener, uint32_t block_frames, uint32_t binaural_budget,
          world::DirectChannel* direct = nullptr, ReflectionRenderer* reflections = nullptr,
          world::PathChannel* paths = nullptr);

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    /// Sets the output format and resets the limiter, the spatial effects and the output FIFO.
    /// Allocates; must not run while anything renders. Voices, buses and gains are kept. Must be
    /// called once before the first render, and after the reflection renderer is prepared.
    /// `device_speakers` (channels entries) is the device's channel order; null = the engine's own
    /// order (Steam Audio's: stereo, quad, 5.1, 7.1), as for the offline output.
    void prepare(uint32_t sample_rate, uint32_t channels, const Speaker* device_speakers = nullptr);

    /// Produces `frames` interleaved frames of any count: whole engine blocks are rendered as
    /// needed and buffered (the FIFO adapter between fixed blocks and device periods). `hook`
    /// runs before each block (the offline path uses it to refill streams synchronously).
    void render(float* out, uint32_t frames, BlockHook hook, void* user) noexcept;

    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t block_frames() const noexcept { return block_frames_; }
    [[nodiscard]] double block_period_seconds() const noexcept {
        return static_cast<double>(block_frames_) / sample_rate_;
    }
    [[nodiscard]] MixerStats& stats() noexcept { return stats_; }

private:
    enum class Generated { Silent, SilentAndEnded, Produced, ProducedAndEnded };

    void render_block() noexcept;
    void apply(const Command& command) noexcept;

    // Voice life cycle (render thread).
    void activate(uint32_t slot) noexcept;
    void start(VoiceSlot& s) noexcept;
    void pause(VoiceSlot& s) noexcept;
    void stop(VoiceSlot& s) noexcept;
    void seek(VoiceSlot& s, double seconds) noexcept;
    void fade(VoiceSlot& s, const Command& command) noexcept;
    /// Returns true if the voice was retired (removed from the active list).
    bool release(uint32_t slot) noexcept;
    void retire(uint32_t slot) noexcept;
    void finish_stop(VoiceSlot& s) noexcept;
    void finish_seek(VoiceSlot& s) noexcept;
    void set_position(VoiceSlot& s, double seconds) noexcept;
    void end_voice(VoiceSlot& s) noexcept;
    void complete_fade(VoiceSlot& s) noexcept;
    void cancel_fade(VoiceSlot& s) noexcept;

    /// Returns true if the voice was retired.
    bool render_voice(uint32_t slot) noexcept;
    Generated generate(VoiceSlot& s) noexcept;
    /// Advances a virtual voice's position exactly as rendering would, without producing audio.
    Generated advance_silent(VoiceSlot& s) noexcept;
    void mix(VoiceSlot& s, const SpatialParams& params, const float* gain, bool positioned) noexcept;
    /// voice_out_ -> mono in voice_out_[0]: stereo halved, a bed averaged over its speaker channels.
    void downmix(const Asset& asset) noexcept;
    /// A bed's channels (gains applied) into bus `b`: from their speakers, or on headphones
    /// binaurally through the head bus.
    void mix_bed(const Asset& asset, std::size_t b) noexcept;
    [[nodiscard]] SpatialParams spatial_params(const RenderVoice& v) const noexcept;
    [[nodiscard]] double playback_ratio(const VoiceSlot& s) const noexcept;
    /// An effect set for `slot`, stealing one from a quieter voice if the pool is empty; -1 if none.
    int acquire_effects(uint32_t slot, float level) noexcept;
    void release_effects(RenderVoice& v) noexcept;
    void update_binaural_threshold() noexcept;
    /// Start of a block: places' levels start over; those nothing has used for a while are let go.
    void update_places() noexcept;
    /// A place for the voice (ADR 0012): the nearest within kPlaceRadius; else a free slot; else
    /// the place unused the longest, or a much quieter one's. -1 if none can be had.
    int assign_place(VoiceSlot& s) noexcept;
    void leave_place(RenderVoice& v) noexcept;
    /// Feeds a positioned voice's signal (all gains but distance applied) to its reflections.
    void send_reflections(VoiceSlot& s, const SpatialParams& params, const float* mono) noexcept;
    /// Renders the reflections and adds them to the output (before the world bus is decoded).
    void mix_reflections() noexcept;
    /// Publishes a world voice's position to the pathing simulation and takes its (smoothed)
    /// results into the set's path state.
    void update_path(VoiceSlot& s, const SpatialParams& params) noexcept;
    /// Renders a voice's signal along the paths found for it into the path bus.
    void send_path(VoiceSlot& s, const SpatialParams& params, const float* mono) noexcept;
    /// Adds the path bus, decoded for the listener, to the output.
    void mix_paths() noexcept;
    /// Publishes a world voice's position to the direct simulation and takes (smoothed) results
    /// into `params`. Returns true while a new voice should wait for its first result.
    bool update_direct(VoiceSlot& s, SpatialParams& params) noexcept;
    void advance_env(RenderVoice& v, uint32_t frames) const noexcept;
    void gather(const Asset& asset, int64_t first, int64_t end, bool loop, bool wrap_before_start) noexcept;
    void publish_position(VoiceSlot& s) noexcept;
    void post_event(vsa_event_type type, vsa_voice voice, uint64_t token, uint32_t flags) noexcept;

    const dsp::ResamplerKernel& kernel_;
    VoiceSlot* slots_;
    uint32_t slot_count_;
    SpscRing<Command>& commands_;
    SpscRing<vsa_event>& events_;
    SpscRing<uint32_t>& retired_;
    RtLog& rt_log_;
    SpatialRenderer& spatial_;
    LatestValue<ListenerPose>& listener_;
    const uint32_t block_frames_;
    ListenerPose pose_;
    vsa_render_mode render_mode_ = VSA_RENDER_HEADPHONES;
    // Binaural budget: voices ranked above binaural_threshold_ (computed at each block start from
    // the previous block's levels) get their own HRTF, the rest share the world ambisonic bus.
    uint32_t binaural_budget_;
    float binaural_threshold_ = 0.0f;
    std::vector<float> ranking_;

    // Direct simulation (Phase 5): per effect set, the generation published with its inputs (bumped
    // when the set changes voices) and the smoothed results.
    struct DirectState {
        float occlusion = 1.0f;
        float transmission[3] = {1.0f, 1.0f, 1.0f};
        bool primed = false;
    };
    world::DirectChannel* direct_;
    ReflectionRenderer* reflections_;
    // Pathing (Phase 7): per effect set, the smoothed path effect parameters; and the order-1
    // world-space Ambisonic bus the path effects render into.
    struct PathState {
        float sh[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float eq[3] = {1.0f, 1.0f, 1.0f};
        bool primed = false;
        bool sounding = false;  // coefficients (still) above nothing: worth rendering
    };
    world::PathChannel* paths_;
    std::vector<PathState> path_state_;
    std::vector<float> path_in_;
    std::vector<float> path_out_storage_;
    std::array<float*, 4> path_out_{};
    std::vector<float> path_bus_storage_;
    std::array<float*, 4> path_bus_{};
    bool path_bus_used_ = false;
    std::unique_ptr<SpeakerDecoder> path_decoder_;
    // Beds (unpositioned sounds of more than two channels) on this output's speakers.
    std::unique_ptr<BedPanner> bed_panner_;
    // Where sounds are simulated from (ADR 0012), one per reflection slot: sounds within
    // kPlaceRadius of a place share its simulation, which is kept (and keeps refining) while
    // sounds keep happening there.
    struct Place {
        bool used = false;
        float position[3] = {};
        uint32_t users = 0;      // voices sending to it now
        uint64_t last_used = 0;  // block of its latest sound
        float level = 0.0f;      // its loudest user this block (level without walls)
        float ranked = 0.0f;     // the same over the last whole block: what a taker must beat
        uint32_t generation = 0;
    };
    std::vector<Place> places_;
    uint64_t block_index_ = 0;
    uint64_t place_idle_blocks_ = 0;
    dsp::GainRamp reflection_gain_;
    std::vector<float> reflection_gain_buf_;
    std::vector<uint32_t> set_generation_;
    std::vector<DirectState> direct_state_;
    float direct_alpha_ = 1.0f;
    uint32_t direct_max_hold_ = 0;

    uint32_t sample_rate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t smooth_frames_ = 240;
    float declick_step_ = 1.0f / 240.0f;
    uint64_t block_period_ns_ = 0;

    // Active voices: dense list of slot indices.
    std::vector<uint32_t> active_;
    uint32_t active_count_ = 0;

    // Scratch, sized once in the constructor.
    std::vector<float> coef_;
    std::vector<float> window_storage_;
    float* window_[decode::kMaxChannels] = {};
    uint32_t window_capacity_ = 0;
    std::vector<float> voice_storage_;
    float* voice_out_[decode::kMaxChannels] = {};
    // Each bus's gain for this block, per frame (ambisonic voices apply it before sharing the bus).
    std::vector<float> bus_gain_storage_;
    std::array<float*, VSA_BUS_COUNT> bus_gains_{};
    std::vector<float> spatial_storage_;
    std::array<float*, kMaxOutputChannels> spatial_out_{};
    std::vector<float> gain_buf_;
    std::vector<float> bus_storage_;
    std::array<float*, VSA_BUS_COUNT * kMaxOutputChannels> bus_{};
    std::array<bool, VSA_BUS_COUNT> bus_used_{};
    std::array<dsp::GainRamp, VSA_BUS_COUNT> bus_gain_{};
    dsp::GainRamp master_gain_;
    std::vector<float> master_storage_;
    std::array<float*, kMaxOutputChannels> master_{};
    std::array<int, kMaxOutputChannels> device_map_{};
    dsp::Limiter limiter_;

    // Output FIFO: one interleaved block.
    std::vector<float> block_out_;
    uint32_t block_read_ = 0;

    MixerStats stats_;
};

}  // namespace vsa
