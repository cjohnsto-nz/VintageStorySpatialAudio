#include "audio/mixer.hpp"

#include "audio/asset.hpp"
#include "audio/stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vsa {
namespace {

constexpr float kMonoPan = 0.70710678f;  // equal power, -3 dB per side
constexpr double kDeclickSeconds = 0.005;
constexpr double kSmoothSeconds = 0.005;

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
             SpscRing<vsa_event>& events, SpscRing<uint32_t>& retired, RtLog& rt_log, uint32_t block_frames)
    : kernel_(kernel),
      slots_(slots),
      slot_count_(slot_count),
      commands_(commands),
      events_(events),
      retired_(retired),
      rt_log_(rt_log),
      block_frames_(block_frames) {
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

    bus_storage_.resize(static_cast<std::size_t>(block_frames) * bus_.size());
    for (std::size_t i = 0; i < bus_.size(); ++i) {
        bus_[i] = bus_storage_.data() + i * block_frames;
    }
    master_storage_.resize(static_cast<std::size_t>(block_frames) * 2);

    prepare(sample_rate_, channels_);
}

void Mixer::prepare(uint32_t sample_rate, uint32_t channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    smooth_frames_ = std::max(1u, static_cast<uint32_t>(std::lround(kSmoothSeconds * sample_rate)));
    declick_step_ = 1.0f / static_cast<float>(std::max(1L, std::lround(kDeclickSeconds * sample_rate)));
    block_period_ns_ = static_cast<uint64_t>(1e9 * block_frames_ / sample_rate);
    limiter_.prepare(sample_rate);
    block_out_.assign(static_cast<std::size_t>(block_frames_) * channels, 0.0f);
    block_read_ = block_frames_;  // empty: the next render() starts a block
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

    bus_used_.fill(false);
    for (uint32_t i = 0; i < active_count_;) {
        if (!render_voice(active_[i])) {
            ++i;  // a retired voice's place is taken by the last one, so do not advance
        }
    }

    float* master_l = master_storage_.data();
    float* master_r = master_storage_.data() + frames;
    std::fill(master_l, master_l + 2 * static_cast<std::size_t>(frames), 0.0f);
    float* gain = gain_buf_.data();
    for (std::size_t b = 0; b < VSA_BUS_COUNT; ++b) {
        if (!bus_used_[b]) {
            bus_gain_[b].render(nullptr, frames);
            continue;
        }
        bus_gain_[b].render(gain, frames);
        const float* bl = bus_[2 * b];
        const float* br = bus_[2 * b + 1];
        for (uint32_t j = 0; j < frames; ++j) {
            master_l[j] += bl[j] * gain[j];
            master_r[j] += br[j] * gain[j];
        }
    }
    master_gain_.render(gain, frames);
    for (uint32_t j = 0; j < frames; ++j) {
        master_l[j] *= gain[j];
        master_r[j] *= gain[j];
    }

    const float limiter_gain = limiter_.process(master_l, master_r, frames);

    float* out = block_out_.data();
    if (channels_ == 2) {
        for (uint32_t j = 0; j < frames; ++j) {
            out[2 * j] = master_l[j];
            out[2 * j + 1] = master_r[j];
        }
    } else {
        std::fill(block_out_.begin(), block_out_.end(), 0.0f);
        for (uint32_t j = 0; j < frames; ++j) {
            out[static_cast<std::size_t>(j) * channels_] = master_l[j];
            out[static_cast<std::size_t>(j) * channels_ + 1] = master_r[j];
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
        // Fades run on wall-clock time even while a voice is silent.
        if (v.gain.render(nullptr, frames)) {
            complete_fade(s);
        }
        publish_position(s);
        return false;
    }

    const Generated generated = generate(s);

    float* gain = gain_buf_.data();
    const bool ramp_done = v.gain.render(gain, frames);
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

    if (generated != Generated::Silent) {
        const std::size_t b = s.bus;
        float* bl = bus_[2 * b];
        float* br = bus_[2 * b + 1];
        if (!bus_used_[b]) {
            std::fill(bl, bl + frames, 0.0f);
            std::fill(br, br + frames, 0.0f);
            bus_used_[b] = true;
        }
        const float* src_l = voice_out_[0];
        if (s.asset->channels() == 1) {
            for (uint32_t j = 0; j < frames; ++j) {
                const float x = src_l[j] * gain[j] * kMonoPan;
                bl[j] += x;
                br[j] += x;
            }
        } else {
            const float* src_r = voice_out_[1];
            for (uint32_t j = 0; j < frames; ++j) {
                bl[j] += src_l[j] * gain[j];
                br[j] += src_r[j] * gain[j];
            }
        }
    }

    if (ramp_done) {
        complete_fade(s);
    }

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
    } else if (generated == Generated::ProducedAndEnded && v.state == VSA_VOICE_PLAYING) {
        end_voice(s);
    }

    publish_position(s);
    return false;
}

Mixer::Generated Mixer::generate(VoiceSlot& s) noexcept {
    RenderVoice& v = s.render;
    const Asset& asset = *s.asset;
    const uint32_t frames = block_frames_;
    const uint32_t channels = asset.channels();
    const double ratio = std::clamp(static_cast<double>(asset.sample_rate()) / sample_rate_ * static_cast<double>(v.pitch), 1e-3,
                                    dsp::ResamplerKernel::kMaxRatio);
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
