#include "audio/mixer.hpp"

#include "audio/asset.hpp"
#include "audio/stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace vsa {
namespace {

constexpr float kMonoPan = 0.70710678f;  // equal power, -3 dB per side
constexpr double kDeclickSeconds = 0.005;
constexpr double kSmoothSeconds = 0.005;
// Virtual below -70 dB estimated output, real again above -64 dB.
constexpr float kVirtualBelow = 3.16e-4f;
constexpr float kRealAbove = 6.3e-4f;
// Steam Audio's default air absorption coefficients (1/m) for its three bands.
constexpr float kAirAbsorption[3] = {0.0002f, 0.0017f, 0.0182f};
// Within this distance of the head a source blends to centred (no meaningful direction).
constexpr float kCentreRadius = 0.5f;
// Direct simulation results: smoothing time, and how long a new voice may wait for its first.
constexpr double kDirectSmoothSeconds = 0.06;
constexpr double kDirectMaxHoldSeconds = 0.08;
// Places (ADR 0012): sounds within this distance of a place share its simulation; a sound takes
// a place in use only if it is this much louder (+6 dB) than the place's loudest user; a place
// nothing has used for this long is let go.
constexpr float kPlaceRadius = 3.0f;
constexpr float kPlaceSteal = 2.0f;
constexpr double kPlaceIdleSeconds = 30.0;

template <typename T>
void store_max(std::atomic<T>& target, T value) noexcept {
    T current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

template <typename T>
void store_min(std::atomic<T>& target, T value) noexcept {
    T current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

Mixer::Mixer(const dsp::ResamplerKernel& kernel, VoiceSlot* slots, uint32_t slot_count, SpscRing<Command>& commands,
             SpscRing<vsa_event>& events, SpscRing<uint32_t>& retired, RtLog& rt_log, SpatialRenderer& spatial,
             LatestValue<ListenerPose>& listener, uint32_t block_frames, uint32_t binaural_budget,
             world::DirectChannel* direct, ReflectionRenderer* reflections)
    : kernel_(kernel),
      slots_(slots),
      slot_count_(slot_count),
      commands_(commands),
      events_(events),
      retired_(retired),
      rt_log_(rt_log),
      spatial_(spatial),
      listener_(listener),
      block_frames_(block_frames),
      binaural_budget_(binaural_budget),
      direct_(direct),
      reflections_(reflections) {
    reflection_gain_buf_.resize(block_frames);
    ranking_.resize(spatial.pool_size());
    set_generation_.assign(spatial.pool_size(), 0);
    direct_state_.resize(spatial.pool_size());
    active_.resize(slot_count);
    coef_.resize(kernel.scratch_floats());

    window_capacity_ = kernel.max_span_frames(block_frames);
    window_storage_.resize(static_cast<std::size_t>(window_capacity_) * 2);
    window_[0] = window_storage_.data();
    window_[1] = window_storage_.data() + window_capacity_;

    voice_storage_.resize(static_cast<std::size_t>(block_frames) * 2);
    voice_out_[0] = voice_storage_.data();
    voice_out_[1] = voice_storage_.data() + block_frames;
    gain_buf_.resize(block_frames);
    bus_gain_storage_.resize(static_cast<std::size_t>(block_frames) * VSA_BUS_COUNT);
    for (std::size_t b = 0; b < VSA_BUS_COUNT; ++b) {
        bus_gains_[b] = bus_gain_storage_.data() + b * block_frames;
    }
    spatial_storage_.resize(static_cast<std::size_t>(block_frames) * kMaxOutputChannels);
    for (std::size_t c = 0; c < kMaxOutputChannels; ++c) {
        spatial_out_[c] = spatial_storage_.data() + c * block_frames;
    }

    bus_storage_.resize(static_cast<std::size_t>(block_frames) * bus_.size());
    for (std::size_t i = 0; i < bus_.size(); ++i) {
        bus_[i] = bus_storage_.data() + i * block_frames;
    }
    master_storage_.resize(static_cast<std::size_t>(block_frames) * kMaxOutputChannels);
    for (std::size_t c = 0; c < kMaxOutputChannels; ++c) {
        master_[c] = master_storage_.data() + c * block_frames;
    }
}

void Mixer::prepare(uint32_t sample_rate, uint32_t channels, const Speaker* device_speakers) {
    sample_rate_ = sample_rate;
    channels_ = std::min(channels, kMaxOutputChannels);
    const std::array<Speaker, kMaxOutputChannels> standard = steam_layout(channels_);
    device_map_ = map_to_device(channels_, device_speakers != nullptr ? device_speakers : standard.data());
    smooth_frames_ = std::max(1u, static_cast<uint32_t>(std::lround(kSmoothSeconds * sample_rate)));
    declick_step_ = 1.0f / static_cast<float>(std::max(1L, std::lround(kDeclickSeconds * sample_rate)));
    block_period_ns_ = static_cast<uint64_t>(1e9 * block_frames_ / sample_rate);
    // Simulation results are smoothed over ~60 ms; a new voice waits up to 80 ms for its first.
    const double block_seconds = static_cast<double>(block_frames_) / sample_rate;
    direct_alpha_ = static_cast<float>(1.0 - std::exp(-block_seconds / kDirectSmoothSeconds));
    direct_max_hold_ = static_cast<uint32_t>(std::ceil(kDirectMaxHoldSeconds / block_seconds));
    place_idle_blocks_ = static_cast<uint64_t>(std::ceil(kPlaceIdleSeconds / block_seconds));
    places_.assign(reflections_ != nullptr && reflections_->enabled() ? reflections_->slot_count() : 0, Place{});
    limiter_.prepare(sample_rate);
    block_out_.assign(static_cast<std::size_t>(block_frames_) * channels, 0.0f);
    block_read_ = block_frames_;  // empty: the next render() starts a block

    // New effect sets for the new rate: every voice re-acquires one on its next block.
    spatial_.prepare(sample_rate, block_frames_, channels_);
    for (uint32_t i = 0; i < active_count_; ++i) {
        RenderVoice& v = slots_[active_[i]].render;
        v.effect_set = -1;
        v.is_virtual = false;
        v.reflection_slot = -1;  // the reflection renderer was prepared afresh
    }
}

void Mixer::render(float* out, uint32_t frames, BlockHook hook, void* user) noexcept {
    uint32_t done = 0;
    while (done < frames) {
        if (block_read_ == block_frames_) {
            if (hook != nullptr) {
                hook(user);
            }
            render_block();
            block_read_ = 0;
        }
        const uint32_t n = std::min(frames - done, block_frames_ - block_read_);
        std::memcpy(out + static_cast<std::size_t>(done) * channels_,
                    block_out_.data() + static_cast<std::size_t>(block_read_) * channels_,
                    static_cast<std::size_t>(n) * channels_ * sizeof(float));
        block_read_ += n;
        done += n;
    }
}

void Mixer::render_block() noexcept {
    const auto started = std::chrono::steady_clock::now();
    const uint32_t frames = block_frames_;

    Command command{};
    while (commands_.try_pop(command)) {
        apply(command);
    }
    pose_ = listener_.read();
    update_binaural_threshold();
    update_places();

    for (std::size_t b = 0; b < VSA_BUS_COUNT; ++b) {
        bus_gain_[b].render(bus_gains_[b], frames);
    }
    spatial_.begin_block();
    if (reflections_ != nullptr) {
        reflections_->begin_block();
    }
    bus_used_.fill(false);
    for (uint32_t i = 0; i < active_count_;) {
        if (!render_voice(active_[i])) {
            ++i;  // a retired voice's place is taken by the last one, so do not advance
        }
    }
    uint32_t virtual_count = 0;
    for (uint32_t i = 0; i < active_count_; ++i) {
        virtual_count += slots_[active_[i]].render.is_virtual ? 1u : 0u;
    }

    // Buses -> master (all output channels, engine order) -> master gain -> linked limiter.
    const uint32_t channels = channels_;
    std::fill(master_storage_.begin(), master_storage_.begin() + static_cast<std::ptrdiff_t>(frames) * channels, 0.0f);
    for (std::size_t b = 0; b < VSA_BUS_COUNT; ++b) {
        if (!bus_used_[b]) {
            continue;
        }
        const float* gain = bus_gains_[b];
        for (uint32_t c = 0; c < channels; ++c) {
            const float* in = bus_[b * kMaxOutputChannels + c];
            float* sum = master_[c];
            for (uint32_t j = 0; j < frames; ++j) {
                sum[j] += in[j] * gain[j];
            }
        }
    }
    mix_reflections();
    // The world ambisonic bus (bus gains already applied), decoded for the listener's head.
    const Orientation orientation{
        {pose_.right[0], pose_.right[1], pose_.right[2]},
        {pose_.up[0], pose_.up[1], pose_.up[2]},
        {pose_.forward[0], pose_.forward[1], pose_.forward[2]},
        {pose_.position[0], pose_.position[1], pose_.position[2]},
    };
    if (spatial_.decode(orientation, spatial_out_[0], spatial_out_[1])) {
        for (uint32_t c = 0; c < 2; ++c) {
            const float* in = spatial_out_[c];
            float* sum = master_[c];
            for (uint32_t j = 0; j < frames; ++j) {
                sum[j] += in[j];
            }
        }
    }

    float* gain = gain_buf_.data();
    master_gain_.render(gain, frames);
    for (uint32_t c = 0; c < channels; ++c) {
        float* x = master_[c];
        for (uint32_t j = 0; j < frames; ++j) {
            x[j] *= gain[j];
        }
    }

    const float limiter_gain = limiter_.process(master_.data(), channels, frames);

    // Interleave into the device's channel order.
    float* out = block_out_.data();
    std::fill(block_out_.begin(), block_out_.end(), 0.0f);
    for (uint32_t c = 0; c < channels; ++c) {
        const int d = device_map_[c];
        if (d < 0) {
            continue;
        }
        const float* x = master_[c];
        for (uint32_t j = 0; j < frames; ++j) {
            out[static_cast<std::size_t>(j) * channels + static_cast<std::size_t>(d)] = x[j];
        }
    }

    const auto elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    stats_.blocks.fetch_add(1, std::memory_order_relaxed);
    stats_.time_sum_ns.fetch_add(elapsed, std::memory_order_relaxed);
    stats_.time_count.fetch_add(1, std::memory_order_relaxed);
    store_max(stats_.time_max_ns, elapsed);
    if (elapsed > block_period_ns_) {
        stats_.overloads.fetch_add(1, std::memory_order_relaxed);
    }
    store_min(stats_.min_limiter_gain, limiter_gain);
    stats_.active_voices.store(active_count_, std::memory_order_relaxed);
    stats_.real_voices.store(spatial_.in_use(), std::memory_order_relaxed);
    stats_.virtual_voices.store(virtual_count, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Commands

void Mixer::apply(const Command& c) noexcept {
    switch (c.op) {
        case Op::SetBusGain:
            if (c.slot < VSA_BUS_COUNT) {
                bus_gain_[c.slot].linear(c.value, smooth_frames_);
            }
            return;
        case Op::SetMasterGain: master_gain_.linear(c.value, smooth_frames_); return;
        case Op::SetReflectionGain: reflection_gain_.linear(c.value, smooth_frames_); return;
        case Op::SetReflectionMix:
            if (reflections_ != nullptr) {
                reflections_->set_mix(c.value, c.seconds, smooth_frames_);
            }
            return;
        case Op::SetRenderMode:
            render_mode_ = c.flags == VSA_RENDER_SPEAKERS ? VSA_RENDER_SPEAKERS : VSA_RENDER_HEADPHONES;
            spatial_.reset_all();  // the other effect kind's state is stale
            return;
        default: break;
    }

    if (c.slot >= slot_count_) {
        rt_log_.post(VSA_LOG_ERROR, "command for an out-of-range voice slot", c.slot, static_cast<int64_t>(c.op));
        return;
    }
    if (c.op == Op::Activate) {
        activate(c.slot);
        return;
    }
    VoiceSlot& s = slots_[c.slot];
    RenderVoice& v = s.render;
    if (!v.active) {
        rt_log_.post(VSA_LOG_ERROR, "command for an inactive voice slot", c.slot, static_cast<int64_t>(c.op));
        return;
    }

    switch (c.op) {
        case Op::Start:
            start(s);
            break;
        case Op::Pause:
            pause(s);
            break;
        case Op::Stop:
            stop(s);
            s.seek_applied.store(c.seek_seq, std::memory_order_release);
            break;
        case Op::SetGain:
            cancel_fade(s);
            v.gain.linear(c.value, smooth_frames_);
            break;
        case Op::SetPitch: v.pitch = c.value; break;
        case Op::SetLooping:
            v.looping = c.value != 0.0f;
            if (s.stream != nullptr) {
                s.stream->set_looping(v.looping);
            }
            break;
        case Op::Seek:
            seek(s, c.position);
            publish_position(s);
            s.seek_applied.store(c.seek_seq, std::memory_order_release);
            break;
        case Op::Fade: fade(s, c); break;
        case Op::SetPosition:
            v.spatial = c.flags;
            v.position[0] = c.vec[0];
            v.position[1] = c.vec[1];
            v.position[2] = c.vec[2];
            break;
        case Op::SetLowpass:
            if (!v.shelf.active() && c.value < 1.0f) {
                v.shelf.reset();
            }
            v.shelf.set(c.value, sample_rate_);
            break;
        case Op::Release: release(c.slot); return;
        default: break;
    }

    if (c.op == Op::Start || c.op == Op::Pause || c.op == Op::Stop) {
        publish_position(s);
        s.render_state.store(v.state, std::memory_order_relaxed);
        s.applied_seq.store(c.seq, std::memory_order_release);
    }
}

void Mixer::activate(uint32_t slot) noexcept {
    VoiceSlot& s = slots_[slot];
    RenderVoice& v = s.render;
    v = RenderVoice{};
    v.active = true;
    v.active_index = active_count_;
    active_[active_count_++] = slot;
    v.gain.jump(s.initial_gain);
    v.pitch = s.initial_pitch;
    v.looping = s.initial_looping;
    v.spatial = s.initial_spatial;
    v.position[0] = s.initial_position[0];
    v.position[1] = s.initial_position[1];
    v.position[2] = s.initial_position[2];
    v.min_distance = s.initial_min_distance;
    v.shelf.set(1.0f, sample_rate_);
    if (s.stream != nullptr) {
        s.stream->set_looping(v.looping);
    }
}

void Mixer::start(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    if (v.state == VSA_VOICE_PLAYING) {
        return;
    }
    // A stop or seek still fading out is completed at once.
    if ((v.pending & RenderVoice::kStop) != 0) {
        finish_stop(s);
    } else if ((v.pending & RenderVoice::kSeek) != 0) {
        finish_seek(s);
    }
    v.pending = static_cast<uint8_t>(v.pending & ~RenderVoice::kPause);
    v.state = VSA_VOICE_PLAYING;
    v.sounded = false;
    v.env_target = 1.0f;  // env == 1 after a clean stop (instant start), 0 after pause/seek (ramp in)
}

void Mixer::pause(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    if (v.state != VSA_VOICE_PLAYING) {
        return;
    }
    v.state = VSA_VOICE_PAUSED;
    v.env_target = 0.0f;
    v.pending = static_cast<uint8_t>(v.pending | RenderVoice::kPause);
}

void Mixer::stop(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    const bool audible = v.state == VSA_VOICE_PLAYING || (v.pending != RenderVoice::kNone && v.env > 0.0f);
    v.state = VSA_VOICE_STOPPED;
    if (audible) {
        v.env_target = 0.0f;
        v.pending = static_cast<uint8_t>((v.pending | RenderVoice::kStop) & ~RenderVoice::kSeek);
    } else {
        finish_stop(s);
    }
}

void Mixer::seek(VoiceSlot& s, double seconds) noexcept {
    RenderVoice& v = s.render;
    v.pending_seek_seconds = seconds;
    const bool audible = v.state == VSA_VOICE_PLAYING || (v.pending != RenderVoice::kNone && v.env > 0.0f);
    if (audible) {
        // Fade out, jump, fade back in. After a pending stop the seek applies once it rewinds.
        v.pending = static_cast<uint8_t>(v.pending | RenderVoice::kSeek);
        v.env_target = 0.0f;
    } else {
        v.pending = static_cast<uint8_t>(v.pending | RenderVoice::kSeek);
        finish_seek(s);
    }
}

void Mixer::finish_stop(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    const bool then_seek = (v.pending & RenderVoice::kSeek) != 0;
    v.pending = RenderVoice::kNone;
    set_position(s, 0.0);
    v.env = 1.0f;  // the next start from the beginning needs no fade-in
    v.env_target = 1.0f;
    if (then_seek) {
        v.pending = RenderVoice::kSeek;
        finish_seek(s);
    }
}

void Mixer::finish_seek(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    v.pending = static_cast<uint8_t>(v.pending & ~RenderVoice::kSeek);
    set_position(s, v.pending_seek_seconds);
    v.env = 0.0f;  // fade in from the new position
    v.env_target = v.state == VSA_VOICE_PLAYING ? 1.0f : 0.0f;
}

void Mixer::set_position(VoiceSlot& s, double seconds) noexcept {
    RenderVoice& v = s.render;
    const Asset& asset = *s.asset;
    const double max_frame = static_cast<double>(asset.frames());
    const double frame = std::clamp(std::floor(seconds * asset.sample_rate()), 0.0, max_frame);
    v.pos = dsp::SourcePosition{};
    v.has_looped = false;
    if (s.stream != nullptr) {
        s.stream->request_seek(static_cast<uint64_t>(frame));
    } else {
        v.pos.frame = static_cast<int64_t>(frame);
    }
}

void Mixer::fade(VoiceSlot& s, const Command& c) noexcept {
    RenderVoice& v = s.render;
    cancel_fade(s);
    const auto frames = static_cast<uint32_t>(std::lround(std::max(0.0f, c.seconds) * static_cast<float>(sample_rate_)));
    v.gain.geometric(c.value, frames);
    v.fade_active = true;
    v.fade_stop = (c.flags & VSA_FADE_STOP_WHEN_DONE) != 0;
    v.fade_token = c.token;
    if (frames == 0) {
        complete_fade(s);
    }
}

void Mixer::complete_fade(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    if (!v.fade_active) {
        return;
    }
    v.fade_active = false;
    post_event(VSA_EVENT_FADE_DONE, s.handle, v.fade_token, 0);
    if (v.fade_stop) {
        stop(s);
        s.render_state.store(v.state, std::memory_order_relaxed);
    }
}

void Mixer::cancel_fade(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    if (v.fade_active) {
        v.fade_active = false;
        post_event(VSA_EVENT_FADE_DONE, s.handle, v.fade_token, VSA_EVENT_FLAG_FADE_CANCELLED);
    }
}

bool Mixer::release(uint32_t slot) noexcept {
    VoiceSlot& s = slots_[slot];
    RenderVoice& v = s.render;
    cancel_fade(s);
    const bool audible = v.state == VSA_VOICE_PLAYING || (v.pending != RenderVoice::kNone && v.env > 0.0f);
    if (audible) {
        v.state = VSA_VOICE_STOPPED;
        v.pending = RenderVoice::kRelease;
        v.env_target = 0.0f;
        return false;
    }
    retire(slot);
    return true;
}

void Mixer::retire(uint32_t slot) noexcept {
    VoiceSlot& s = slots_[slot];
    RenderVoice& v = s.render;
    cancel_fade(s);
    release_effects(v);
    const uint32_t index = v.active_index;
    const uint32_t last = active_[--active_count_];
    active_[index] = last;
    slots_[last].render.active_index = index;
    v.active = false;
    if (!retired_.try_push(slot)) {
        rt_log_.post(VSA_LOG_ERROR, "retired-voice queue full; slot leaked", slot);
    }
}

void Mixer::end_voice(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    v.state = VSA_VOICE_STOPPED;
    v.pending = RenderVoice::kNone;
    set_position(s, 0.0);
    v.env = 1.0f;
    v.env_target = 1.0f;
    s.render_state.store(VSA_VOICE_STOPPED, std::memory_order_relaxed);
    post_event(VSA_EVENT_VOICE_ENDED, s.handle, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Rendering

bool Mixer::render_voice(uint32_t slot) noexcept {
    VoiceSlot& s = slots_[slot];
    RenderVoice& v = s.render;
    const uint32_t frames = block_frames_;

    const bool audible = v.state == VSA_VOICE_PLAYING || (v.pending != RenderVoice::kNone && v.env > 0.0f);
    if (!audible) {
        release_effects(v);
        v.is_virtual = false;
        // Fades run on wall-clock time even while a voice is silent.
        if (v.gain.render(nullptr, frames)) {
            complete_fade(s);
        }
        publish_position(s);
        return false;
    }

    // Estimate the output level; voices far below audibility go virtual (with hysteresis), and
    // positional voices need an effect set to be heard.
    const bool spatial = v.spatial != VSA_SPATIAL_NONE;
    SpatialParams params;
    float level = v.gain.value() * bus_gain_[s.bus].value() * master_gain_.value();
    if (spatial) {
        params = spatial_params(v);
        level *= params.distance_gain;
    } else {
        release_effects(v);
    }
    // Ranking (binaural budget, effect-set stealing) counts walls: a voice muffled behind rock
    // ranks low. Virtualisation does not: an occluded voice stays real, so it keeps being
    // simulated and is heard again the moment a door opens.
    v.level = level;
    v.open_level = level;
    if (spatial && v.effect_set >= 0 && direct_state_[static_cast<std::size_t>(v.effect_set)].primed) {
        const DirectState& d = direct_state_[static_cast<std::size_t>(v.effect_set)];
        v.level = level * (d.occlusion + (1.0f - d.occlusion) * d.transmission[1]);
    }
    // Streams are never virtual (skipping ahead would need a seek on the decoder thread); one
    // that cannot get an effect set plays unpositioned until it can.
    const bool streamed = s.stream != nullptr;
    bool silent = !streamed && level < (v.is_virtual ? kRealAbove : kVirtualBelow);
    if (!silent && spatial && v.effect_set < 0) {
        v.effect_set = acquire_effects(slot, v.level);
        silent = v.effect_set < 0 && !streamed;
    }
    if (silent) {
        release_effects(v);
    }
    v.is_virtual = silent;
    const bool positioned = spatial && v.effect_set >= 0;
    if (positioned) {
        SpatialTier tier = SpatialTier::Panned;
        if (render_mode_ == VSA_RENDER_HEADPHONES) {
            const float ranked = v.level * (v.tier == SpatialTier::Binaural ? 2.0f : 1.0f);
            tier = ranked >= binaural_threshold_ ? SpatialTier::Binaural : SpatialTier::Ambisonic;
        }
        if (tier != v.tier) {
            spatial_.reset_tier(v.effect_set, tier);  // its state is from an earlier use
            v.tier = tier;
        }
        if (update_direct(s, params)) {
            publish_position(s);  // held until its first simulation result
            return false;
        }
    }

    const Generated generated = silent ? advance_silent(s) : generate(s);

    float* gain = gain_buf_.data();
    bool ramp_done = false;
    if (silent) {
        ramp_done = v.gain.render(nullptr, frames);
        advance_env(v, frames);
    } else {
        ramp_done = v.gain.render(gain, frames);
        if (v.env != 1.0f || v.env_target != 1.0f) {
            float env = v.env;
            const float target = v.env_target;
            const float step = declick_step_;
            for (uint32_t j = 0; j < frames; ++j) {
                env = env < target ? std::min(target, env + step) : std::max(target, env - step);
                gain[j] *= env;
            }
            v.env = env;
        }
    }

    if (generated == Generated::Produced || generated == Generated::ProducedAndEnded) {
        mix(s, params, gain, positioned);
        v.sounded = true;
    }

    if (ramp_done) {
        complete_fade(s);
    }

    const bool ended = generated == Generated::ProducedAndEnded || generated == Generated::SilentAndEnded;
    if (v.env == 0.0f && v.pending != RenderVoice::kNone) {
        if ((v.pending & RenderVoice::kRelease) != 0) {
            retire(slot);
            return true;
        }
        if ((v.pending & RenderVoice::kStop) != 0) {
            finish_stop(s);
        } else if ((v.pending & RenderVoice::kSeek) != 0) {
            finish_seek(s);
        }
        v.pending = static_cast<uint8_t>(v.pending & ~RenderVoice::kPause);
    } else if (ended && v.state == VSA_VOICE_PLAYING) {
        end_voice(s);
    }

    publish_position(s);
    return false;
}

void Mixer::mix(VoiceSlot& s, const SpatialParams& params, const float* gain, bool positioned) noexcept {
    RenderVoice& v = s.render;
    const uint32_t frames = block_frames_;
    const uint32_t channels = s.asset->channels();
    float* left = voice_out_[0];
    float* right = voice_out_[1];
    if (v.shelf.active() && v.shelf.rate() != sample_rate_) {
        v.shelf.set(v.shelf.gain(), sample_rate_);  // the output rate changed
    }

    const std::size_t b = s.bus;
    float* const* bus = &bus_[b * kMaxOutputChannels];
    float* bl = bus[0];
    float* br = bus[1];
    if (!bus_used_[b]) {
        for (uint32_t c = 0; c < channels_; ++c) {
            std::fill(bus[c], bus[c] + frames, 0.0f);
        }
        bus_used_[b] = true;
    }

    if (positioned) {
        // Positioned: mono (stereo assets downmixed) -> gain -> shelf -> Steam Audio -> stereo.
        if (channels == 2) {
            for (uint32_t j = 0; j < frames; ++j) {
                left[j] = 0.5f * (left[j] + right[j]) * gain[j];
            }
        } else {
            for (uint32_t j = 0; j < frames; ++j) {
                left[j] *= gain[j];
            }
        }
        if (v.shelf.active()) {
            v.shelf.process(left, frames, 0);
        }
        send_reflections(s, params, left);
        if (v.tier == SpatialTier::Ambisonic) {
            // The shared bus is decoded after every bus gain, so this voice's is applied now.
            const float* bus_gain = bus_gains_[b];
            for (uint32_t j = 0; j < frames; ++j) {
                left[j] *= bus_gain[j];
            }
            spatial_.encode(v.effect_set, params, left);
            return;
        }
        // Binaural goes to the front pair; panning covers the whole output layout.
        const bool binaural = v.tier == SpatialTier::Binaural;
        spatial_.render(v.effect_set, binaural ? VSA_RENDER_HEADPHONES : VSA_RENDER_SPEAKERS, params, left,
                        spatial_out_.data());
        const uint32_t count = binaural ? 2u : spatial_.speaker_channels();
        for (uint32_t c = 0; c < count; ++c) {
            const float* in = spatial_out_[c];
            float* sum = bus[c];
            for (uint32_t j = 0; j < frames; ++j) {
                sum[j] += in[j];
            }
        }
        return;
    }

    for (uint32_t c = 0; c < channels; ++c) {
        float* x = voice_out_[c];
        for (uint32_t j = 0; j < frames; ++j) {
            x[j] *= gain[j];
        }
        if (v.shelf.active()) {
            v.shelf.process(x, frames, c);
        }
    }
    if (channels == 1) {
        for (uint32_t j = 0; j < frames; ++j) {
            const float x = left[j] * kMonoPan;
            bl[j] += x;
            br[j] += x;
        }
    } else {
        for (uint32_t j = 0; j < frames; ++j) {
            bl[j] += left[j];
            br[j] += right[j];
        }
    }
}

SpatialParams Mixer::spatial_params(const RenderVoice& v) const noexcept {
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float local[3] = {v.position[0], v.position[1], v.position[2]};
    if (v.spatial == VSA_SPATIAL_WORLD) {
        // Rendering listens from the offset position (e.g. a little behind the eyes).
        const float d[3] = {v.position[0] - (pose_.position[0] + pose_.render_offset[0]),
                            v.position[1] - (pose_.position[1] + pose_.render_offset[1]),
                            v.position[2] - (pose_.position[2] + pose_.render_offset[2])};
        // Listener space as Steam Audio expects it: +x right, +y up, -z forward.
        local[0] = dot(d, pose_.right);
        local[1] = dot(d, pose_.up);
        local[2] = -dot(d, pose_.forward);
    }
    const float distance = std::sqrt(dot(local, local));

    SpatialParams p;
    p.distance = distance;
    p.distance_gain = std::min(1.0f, v.min_distance / std::max(distance, 1e-6f));
    for (std::size_t band = 0; band < 3; ++band) {
        p.air_absorption[band] = std::exp(-kAirAbsorption[band] * distance);
    }
    if (distance > 1e-4f) {
        p.direction[0] = local[0] / distance;
        p.direction[1] = local[1] / distance;
        p.direction[2] = local[2] / distance;
    }
    // A source at the head has no direction: blend to centred within kCentreRadius.
    p.spatial_blend = std::min(1.0f, distance / kCentreRadius);
    // The same direction in world space, for the world ambisonic bus (listener space is +x right,
    // +y up, -z forward).
    for (std::size_t i = 0; i < 3; ++i) {
        p.world_direction[i] =
            pose_.right[i] * p.direction[0] + pose_.up[i] * p.direction[1] - pose_.forward[i] * p.direction[2];
    }
    return p;
}

int Mixer::acquire_effects(uint32_t slot, float level) noexcept {
    int set = spatial_.acquire();
    if (set < 0) {
        // Pool exhausted: take the set of the quietest positional voice, if it is quieter.
        uint32_t victim = slot_count_;
        float lowest = level;
        for (uint32_t i = 0; i < active_count_; ++i) {
            const uint32_t other = active_[i];
            const RenderVoice& o = slots_[other].render;
            if (other != slot && o.effect_set >= 0 && o.level < lowest) {
                lowest = o.level;
                victim = other;
            }
        }
        if (victim == slot_count_) {
            return -1;
        }
        RenderVoice& loser = slots_[victim].render;
        set = loser.effect_set;
        loser.effect_set = -1;
        loser.is_virtual = true;
    }
    spatial_.reset(set);
    if (direct_ != nullptr) {
        uint32_t& generation = set_generation_[static_cast<std::size_t>(set)];
        generation = generation + 1 == 0 ? 1 : generation + 1;
        direct_state_[static_cast<std::size_t>(set)] = DirectState{};
    }
    slots_[slot].render.direct_hold = 0;
    return set;
}

bool Mixer::update_direct(VoiceSlot& s, SpatialParams& params) noexcept {
    RenderVoice& v = s.render;
    if (direct_ == nullptr || v.effect_set < 0) {
        return false;
    }
    const auto set = static_cast<uint32_t>(v.effect_set);
    world::DirectInput& in = direct_->input(set);
    if (v.spatial != VSA_SPATIAL_WORLD) {
        in.generation.store(0, std::memory_order_release);  // head-locked: nothing in the way
        return false;
    }
    const uint32_t generation = set_generation_[set];
    in.x.store(v.position[0], std::memory_order_relaxed);
    in.y.store(v.position[1], std::memory_order_relaxed);
    in.z.store(v.position[2], std::memory_order_relaxed);
    in.voice.store(s.handle, std::memory_order_relaxed);
    in.generation.store(generation, std::memory_order_release);

    DirectState& state = direct_state_[set];
    const world::DirectOutput& out = direct_->output(set);
    if (out.generation.load(std::memory_order_acquire) == generation) {
        const float occlusion = out.occlusion.load(std::memory_order_relaxed);
        float transmission[3];
        for (int b = 0; b < 3; ++b) {
            transmission[b] = out.transmission[b].load(std::memory_order_relaxed);
        }
        if (!state.primed) {
            state.occlusion = occlusion;
            std::copy_n(transmission, 3, state.transmission);
            state.primed = true;
        } else {
            state.occlusion += direct_alpha_ * (occlusion - state.occlusion);
            for (int b = 0; b < 3; ++b) {
                state.transmission[b] += direct_alpha_ * (transmission[b] - state.transmission[b]);
            }
        }
    } else if (!state.primed && !v.sounded && v.direct_hold < direct_max_hold_ &&
               direct_->scene_has_chunks.load(std::memory_order_relaxed)) {
        ++v.direct_hold;
        return true;
    }
    params.occlusion = state.occlusion;
    std::copy_n(state.transmission, 3, params.transmission);
    return false;
}

void Mixer::update_binaural_threshold() noexcept {
    // Rank last block's levels of voices holding effects; current binaural voices get +6 dB so
    // voices near the cut-off do not flip between binaural and ambisonic every block.
    uint32_t count = 0;
    for (uint32_t i = 0; i < active_count_ && count < ranking_.size(); ++i) {
        const RenderVoice& v = slots_[active_[i]].render;
        if (v.effect_set >= 0) {
            ranking_[count++] = v.level * (v.tier == SpatialTier::Binaural ? 2.0f : 1.0f);
        }
    }
    if (count <= binaural_budget_) {
        binaural_threshold_ = 0.0f;
        return;
    }
    if (binaural_budget_ == 0) {
        binaural_threshold_ = std::numeric_limits<float>::infinity();
        return;
    }
    const auto begin = ranking_.begin();
    std::nth_element(begin, begin + (binaural_budget_ - 1), begin + count, std::greater<float>());
    binaural_threshold_ = ranking_[binaural_budget_ - 1];
}

void Mixer::release_effects(RenderVoice& v) noexcept {
    leave_place(v);
    if (v.effect_set >= 0) {
        if (direct_ != nullptr) {
            direct_->input(static_cast<uint32_t>(v.effect_set)).generation.store(0, std::memory_order_release);
        }
        spatial_.release(v.effect_set);
        v.effect_set = -1;
    }
}

void Mixer::leave_place(RenderVoice& v) noexcept {
    if (v.reflection_slot > 0) {
        Place& p = places_[static_cast<std::size_t>(v.reflection_slot)];
        if (p.generation == v.place_generation && p.users > 0) {
            --p.users;
        }
    }
    v.reflection_slot = -1;
}

void Mixer::update_places() noexcept {
    ++block_index_;
    for (std::size_t i = 1; i < places_.size(); ++i) {
        Place& p = places_[i];
        p.ranked = p.level;
        p.level = 0.0f;
        if (p.used && p.users == 0 && block_index_ - p.last_used > place_idle_blocks_) {
            reflections_->release(static_cast<int>(i));
            p.used = false;
        }
    }
}

int Mixer::assign_place(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    int nearest = -1;
    float nearest_d2 = kPlaceRadius * kPlaceRadius;
    for (std::size_t i = 1; i < places_.size(); ++i) {
        const Place& p = places_[i];
        if (!p.used) {
            continue;
        }
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = p.position[k] - v.position[k];
            d2 += d * d;
        }
        if (d2 <= nearest_d2) {
            nearest_d2 = d2;
            nearest = static_cast<int>(i);
        }
    }
    if (nearest > 0) {
        Place& p = places_[static_cast<std::size_t>(nearest)];
        ++p.users;
        v.reflection_slot = nearest;
        v.place_generation = p.generation;
        return nearest;
    }
    int slot = reflections_->acquire(s.handle);
    if (slot < 0) {
        // The place unused the longest; failing that, a place in use whose loudest sound is well
        // below this one (its users lose their reflections until they find another place).
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        float quietest = std::numeric_limits<float>::infinity();
        int quiet = -1;
        for (std::size_t i = 1; i < places_.size(); ++i) {
            const Place& p = places_[i];
            if (!p.used) {
                continue;
            }
            if (p.users == 0 && p.last_used < oldest) {
                oldest = p.last_used;
                slot = static_cast<int>(i);
            } else if (p.users > 0 && p.ranked < quietest) {
                quietest = p.ranked;
                quiet = static_cast<int>(i);
            }
        }
        if (slot < 0 && quiet > 0 && v.open_level > kPlaceSteal * quietest) {
            slot = quiet;
        }
        if (slot < 0) {
            return -1;
        }
        reflections_->reassign(slot, s.handle);
    }
    Place& p = places_[static_cast<std::size_t>(slot)];
    p.used = true;
    std::copy_n(v.position, 3, p.position);
    p.users = 1;
    p.level = v.open_level;
    p.ranked = v.open_level;
    p.generation = p.generation + 1 == 0 ? 1 : p.generation + 1;
    reflections_->set_position(slot, v.position);
    v.reflection_slot = slot;
    v.place_generation = p.generation;
    return slot;
}

void Mixer::send_reflections(VoiceSlot& s, const SpatialParams& params, const float* mono) noexcept {
    if (reflections_ == nullptr || !reflections_->enabled()) {
        return;
    }
    RenderVoice& v = s.render;
    int slot = 0;  // head-locked sounds: the listener's own reverb
    if (v.spatial == VSA_SPATIAL_WORLD) {
        slot = v.reflection_slot;
        if (slot > 0 && places_[static_cast<std::size_t>(slot)].generation != v.place_generation) {
            v.reflection_slot = -1;  // the place was taken over
            slot = -1;
        }
        if (slot > 0) {
            Place& p = places_[static_cast<std::size_t>(slot)];
            if (p.users == 1) {
                // Alone at its place, a moving sound takes the place with it.
                std::copy_n(v.position, 3, p.position);
                reflections_->set_position(slot, v.position);
            } else {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float d = p.position[k] - v.position[k];
                    d2 += d * d;
                }
                if (d2 > kPlaceRadius * kPlaceRadius) {
                    leave_place(v);  // moved away from a shared place: find another
                    slot = -1;
                }
            }
        }
        if (slot < 0) {
            slot = assign_place(s);
        }
        if (slot < 0) {
            return;  // no place to be had: this sound has no reflections
        }
        Place& p = places_[static_cast<std::size_t>(slot)];
        p.last_used = block_index_;
        p.level = std::max(p.level, v.open_level);
    }
    // Steam Audio simulates a source that plays at 1 at 1 m (1 / distance beyond); our direct path
    // plays at 1 within min_distance (min_distance / distance beyond). The ratio of the two scales
    // its reflections to this sound. Sent from the first block: a waiting place's effects take it
    // into their history.
    const float loudness = std::clamp(params.distance, 1.0f, std::max(1.0f, v.min_distance));
    const float* bus_gain = bus_gains_[s.bus];
    float* send = reflections_->send(slot);
    for (uint32_t j = 0; j < block_frames_; ++j) {
        send[j] += mono[j] * bus_gain[j] * loudness;
    }
}

void Mixer::mix_reflections() noexcept {
    if (reflections_ == nullptr || !reflections_->enabled()) {
        return;
    }
    float* gain = reflection_gain_buf_.data();
    reflection_gain_.render(gain, block_frames_);
    if (!reflections_->render(gain)) {
        return;
    }
    if (render_mode_ == VSA_RENDER_HEADPHONES) {
        spatial_.add_ambisonic(reflections_->bus(), reflections_->bus_channels());
    } else {
        reflections_->decode_speakers(pose_.right, pose_.up, pose_.forward, master_.data());
    }
}

void Mixer::advance_env(RenderVoice& v, uint32_t frames) const noexcept {
    const float step = declick_step_ * static_cast<float>(frames);
    v.env = v.env < v.env_target ? std::min(v.env_target, v.env + step) : std::max(v.env_target, v.env - step);
}

double Mixer::playback_ratio(const VoiceSlot& s) const noexcept {
    return std::clamp(static_cast<double>(s.asset->sample_rate()) / sample_rate_ * static_cast<double>(s.render.pitch),
                      1e-3, dsp::ResamplerKernel::kMaxRatio);
}

Mixer::Generated Mixer::advance_silent(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    dsp::ResamplerKernel::advance(v.pos, playback_ratio(s), block_frames_);
    const auto length = static_cast<int64_t>(s.asset->frames());
    if (v.pos.frame >= length) {
        if (!v.looping) {
            return Generated::SilentAndEnded;
        }
        v.pos.frame %= length;
        v.has_looped = true;
    }
    return Generated::Silent;
}
Mixer::Generated Mixer::generate(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    const Asset& asset = *s.asset;
    const uint32_t frames = block_frames_;
    const uint32_t channels = asset.channels();
    const double ratio = playback_ratio(s);
    const dsp::ResamplerKernel::Span span = kernel_.span(v.pos, ratio, frames);

    if (s.stream != nullptr) {
        Stream& stream = *s.stream;
        switch (stream.prepare_window(span.first, span.end)) {
            case Stream::Status::Waiting: return Generated::Silent;
            case Stream::Status::Starved:
                if (!v.underrun) {
                    v.underrun = true;
                    stats_.underruns.fetch_add(1, std::memory_order_relaxed);
                    post_event(VSA_EVENT_STREAM_UNDERRUN, s.handle, 0, 0);
                }
                return Generated::Silent;
            case Stream::Status::Ready: v.underrun = false; break;
        }
        kernel_.process(stream.window(), stream.window_first(), channels, v.pos, ratio, voice_out_, frames,
                        coef_.data());
        return v.pos.frame >= stream.end_virtual() ? Generated::ProducedAndEnded : Generated::Produced;
    }

    if (span.end - span.first > static_cast<int64_t>(window_capacity_)) {
        rt_log_.post(VSA_LOG_ERROR, "resampler span exceeds the window", span.end - span.first, window_capacity_);
        return Generated::Silent;
    }
    gather(asset, span.first, span.end, v.looping, v.has_looped);
    kernel_.process(window_, span.first, channels, v.pos, ratio, voice_out_, frames, coef_.data());

    const auto length = static_cast<int64_t>(asset.frames());
    if (v.pos.frame >= length) {
        if (!v.looping) {
            return Generated::ProducedAndEnded;
        }
        v.pos.frame %= length;
        v.has_looped = true;
    }
    return Generated::Produced;
}

void Mixer::gather(const Asset& asset, int64_t first, int64_t end, bool loop, bool wrap_before_start) noexcept {
    constexpr float kScale = 1.0f / 32768.0f;
    const auto length = static_cast<int64_t>(asset.frames());
    const uint32_t channels = asset.channels();
    const int16_t* pcm = asset.pcm();
    float* left = window_[0];
    float* right = window_[1];

    const auto copy = [&](int64_t src, int64_t count, std::size_t out) {
        const int16_t* p = pcm + static_cast<std::size_t>(src) * channels;
        if (channels == 1) {
            for (int64_t r = 0; r < count; ++r) {
                left[out + static_cast<std::size_t>(r)] = static_cast<float>(p[r]) * kScale;
            }
        } else {
            for (int64_t r = 0; r < count; ++r) {
                left[out + static_cast<std::size_t>(r)] = static_cast<float>(p[2 * r]) * kScale;
                right[out + static_cast<std::size_t>(r)] = static_cast<float>(p[2 * r + 1]) * kScale;
            }
        }
    };
    const auto zero = [&](int64_t count, std::size_t out) {
        std::fill(left + out, left + out + count, 0.0f);
        if (channels == 2) {
            std::fill(right + out, right + out + count, 0.0f);
        }
    };

    int64_t i = first;
    std::size_t out = 0;
    while (i < end) {
        int64_t run = 0;
        if (loop && (i >= 0 || wrap_before_start)) {
            const int64_t src = ((i % length) + length) % length;
            run = std::min(end - i, length - src);
            copy(src, run, out);
        } else if (i < 0) {
            run = std::min<int64_t>(end, 0) - i;
            zero(run, out);
        } else if (i >= length) {
            run = end - i;
            zero(run, out);
        } else {
            run = std::min(end, length) - i;
            copy(i, run, out);
        }
        i += run;
        out += static_cast<std::size_t>(run);
    }
}

void Mixer::publish_position(VoiceSlot& s) noexcept {
    const RenderVoice& v = s.render;
    double seconds = 0.0;
    if ((v.pending & RenderVoice::kStop) != 0) {
        seconds = (v.pending & RenderVoice::kSeek) != 0 ? v.pending_seek_seconds : 0.0;
    } else if ((v.pending & RenderVoice::kSeek) != 0) {
        seconds = v.pending_seek_seconds;
    } else {
        const Asset& asset = *s.asset;
        double frame = static_cast<double>(v.pos.frame) + v.pos.fraction;
        if (s.stream != nullptr) {
            frame += static_cast<double>(s.stream->base_frame());
            const auto length = static_cast<double>(asset.frames());
            if (frame >= length) {
                frame = std::fmod(frame, length);
            }
        }
        seconds = frame / asset.sample_rate();
    }
    s.position.store(seconds, std::memory_order_relaxed);
}

void Mixer::post_event(vsa_event_type type, vsa_voice voice, uint64_t token, uint32_t flags) noexcept {
    vsa_event event{};
    event.struct_size = sizeof event;
    event.type = static_cast<uint32_t>(type);
    event.voice = voice;
    event.token = token;
    event.flags = flags;
    if (!events_.try_push(event)) {
        stats_.events_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace vsa
