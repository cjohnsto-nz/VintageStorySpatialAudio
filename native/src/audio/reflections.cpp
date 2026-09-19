#include "audio/reflections.hpp"

#include "core/error.hpp"
#include "dsp/spherical_harmonics.hpp"
#include "steam/steam_context.hpp"
#include "world/reflection_channel.hpp"
#include "world/reflection_sim.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace vsa {
namespace {

void check(IPLerror error, const char* what) {
    if (error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string(what) + " failed: " + steam::error_name(error));
    }
}

// The late reverb's outputs arrive from the corners of a cube around the listener.
constexpr float kCorner = 0.57735027f;
constexpr float kLateDirections[dsp::LateReverb::kOutputs][3] = {
    {kCorner, kCorner, kCorner},   {-kCorner, kCorner, kCorner},   {kCorner, -kCorner, kCorner},
    {-kCorner, -kCorner, kCorner}, {kCorner, kCorner, -kCorner},   {-kCorner, kCorner, -kCorner},
    {kCorner, -kCorner, -kCorner}, {-kCorner, -kCorner, -kCorner},
};

constexpr double kMeterSeconds = 0.3;


}  // namespace

ReflectionRenderer::ReflectionRenderer(const steam::SteamContext& steam, world::ReflectionChannel* channel,
                                       uint32_t block_frames)
    : steam_(steam), channel_(channel), frames_(block_frames) {}

ReflectionRenderer::~ReflectionRenderer() = default;

void ReflectionRenderer::prepare(uint32_t sample_rate, uint32_t channels, world::ReflectionSimulator* simulator) {
    for (std::size_t i = 0; i < slots_.size() && i < generations_.size(); ++i) {
        generations_[i] = slots_[i].generation;
    }
    slots_.clear();
    decoder_.reset();
    simulator_ = nullptr;
    sample_rate_ = sample_rate;
    bus_used_ = false;
    mean_square_ = 0.0f;
    meter_.mean_square.store(0.0f, std::memory_order_relaxed);
    if (simulator == nullptr || channel_ == nullptr) {
        return;
    }
    channels_ = simulator->ir_channels();
    const auto early_samples = static_cast<uint32_t>(
        std::ceil(static_cast<double>(simulator->settings().transition) * sample_rate));
    early_blocks_ = (early_samples + frames_ - 1) / frames_ + 1;

    IPLAudioSettings audio{};
    audio.samplingRate = static_cast<IPLint32>(sample_rate);
    audio.frameSize = static_cast<IPLint32>(frames_);
    IPLReflectionEffectSettings effect{};
    // Only the convolved part: the simulator's impulse responses are silent after the transition
    // (Steam Audio's hybrid estimate), and the tail is our own LateReverb.
    effect.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    effect.irSize = static_cast<IPLint32>(early_samples);
    effect.numChannels = static_cast<IPLint32>(channels_);

    slots_.resize(simulator->slot_count());
    generations_.resize(slots_.size(), 0);
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        check(iplReflectionEffectCreate(steam_.context(), &audio, &effect, slot.early.out()), "iplReflectionEffectCreate");
        slot.late.prepare(sample_rate, early_samples + frames_);
        slot.ir = simulator->ir(i);
        slot.send.assign(frames_, 0.0f);
        slot.state = i == 0 ? State::Waiting : State::Free;
        slot.generation = i == 0 ? world::ReflectionChannel::kListenerGeneration : generations_[i];
        if (i > 0) {
            channel_->input(i).generation.store(0, std::memory_order_release);
        }
    }
    silence_.assign(frames_, 0.0f);
    early_gain_buf_.assign(frames_, 1.0f);
    tail_gain_buf_.assign(frames_, 1.0f);
    early_storage_.assign(static_cast<std::size_t>(frames_) * channels_, 0.0f);
    for (uint32_t c = 0; c < channels_; ++c) {
        early_out_[c] = early_storage_.data() + static_cast<std::size_t>(c) * frames_;
    }
    late_storage_.assign(static_cast<std::size_t>(frames_) * dsp::LateReverb::kOutputs, 0.0f);
    for (int k = 0; k < dsp::LateReverb::kOutputs; ++k) {
        late_out_[static_cast<std::size_t>(k)] = late_storage_.data() + static_cast<std::size_t>(k) * frames_;
    }
    bus_storage_.assign(static_cast<std::size_t>(frames_) * channels_, 0.0f);
    for (uint32_t c = 0; c < channels_; ++c) {
        bus_[c] = bus_storage_.data() + static_cast<std::size_t>(c) * frames_;
    }
    // Each late output is a plane wave at 1/sqrt(n): the outputs are uncorrelated, so their
    // pressure (W) carries the power of one output, as Steam Audio's mono tail would.
    const float scale = 1.0f / std::sqrt(static_cast<float>(dsp::LateReverb::kOutputs));
    for (int k = 0; k < dsp::LateReverb::kOutputs; ++k) {
        float sh[dsp::kSh3Channels];
        dsp::sh_order3(kLateDirections[k], sh);
        for (uint32_t c = 0; c < channels_; ++c) {
            late_sh_[static_cast<std::size_t>(k)][c] = sh[c] * scale;
        }
    }
    decoder_ = std::make_unique<SpeakerDecoder>(channels, static_cast<int>(simulator->settings().order));
    meter_alpha_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(frames_) / (kMeterSeconds * sample_rate)));
    simulator_ = simulator;
}

void ReflectionRenderer::set_mix(float early, float tail, uint32_t frames) noexcept {
    early_gain_.linear(early, frames);
    tail_gain_.linear(tail, frames);
}

void ReflectionRenderer::begin_block() noexcept {
    for (Slot& slot : slots_) {
        if (slot.sent) {
            std::fill(slot.send.begin(), slot.send.end(), 0.0f);
            slot.sent = false;
        }
    }
}

int ReflectionRenderer::acquire(vsa_voice voice) noexcept {
    for (uint32_t i = 1; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.state != State::Free) {
            continue;
        }
        slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
        slot.state = State::Waiting;
        slot.sent = false;
        world::ReflectionInput& in = channel_->input(i);
        in.voice.store(voice, std::memory_order_relaxed);
        return static_cast<int>(i);
    }
    return -1;
}

void ReflectionRenderer::set_position(int slot, const float position[3]) noexcept {
    const auto i = static_cast<uint32_t>(slot);
    world::ReflectionInput& in = channel_->input(i);
    in.x.store(position[0], std::memory_order_relaxed);
    in.y.store(position[1], std::memory_order_relaxed);
    in.z.store(position[2], std::memory_order_relaxed);
    // Published after the position (the first time: the slot starts being simulated).
    const uint32_t generation = slots_[i].generation;
    if (in.generation.load(std::memory_order_relaxed) != generation) {
        in.generation.store(generation, std::memory_order_release);
    }
}

void ReflectionRenderer::release(int slot) noexcept {
    if (slot <= 0) {
        return;
    }
    const auto i = static_cast<uint32_t>(slot);
    Slot& s = slots_[i];
    channel_->input(i).generation.store(0, std::memory_order_release);
    s.state = State::Draining;
    s.flushed = false;
}

bool ReflectionRenderer::ready(int slot) const noexcept {
    return slot >= 0 && slots_[static_cast<std::size_t>(slot)].state == State::Live;
}

float* ReflectionRenderer::send(int slot) noexcept {
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    s.sent = true;
    return s.send.data();
}

uint32_t ReflectionRenderer::free_slots() const noexcept {
    uint32_t count = 0;
    for (std::size_t i = 1; i < slots_.size(); ++i) {
        count += slots_[i].state == State::Free ? 1u : 0u;
    }
    return count;
}

bool ReflectionRenderer::take_results(uint32_t index, Slot& slot) noexcept {
    const world::ReflectionOutput& out = channel_->output(index);
    if (out.generation.load(std::memory_order_acquire) != slot.generation) {
        return false;
    }
    for (int b = 0; b < 3; ++b) {
        slot.reverb_times[b] = out.reverb_times[b].load(std::memory_order_relaxed);
        slot.eq[b] = out.eq[b].load(std::memory_order_relaxed);
    }
    slot.delay = out.delay.load(std::memory_order_relaxed);
    return true;
}

void ReflectionRenderer::render_slot(Slot& slot, bool input, bool flush) noexcept {
    const float* in = input ? slot.send.data() : silence_.data();
    const auto frames = static_cast<IPLint32>(frames_);
    if (input || slot.early_tail > 0 || flush) {
        float* in_channels[1] = {const_cast<float*>(in)};
        IPLAudioBuffer in_buffer{1, frames, in_channels};
        IPLAudioBuffer out_buffer{static_cast<IPLint32>(channels_), frames, early_out_.data()};
        IPLReflectionEffectParams params{};
        params.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        params.ir = slot.ir;
        params.numChannels = static_cast<IPLint32>(channels_);
        params.irSize = static_cast<IPLint32>(early_blocks_ * frames_);
        iplReflectionEffectApply(slot.early.get(), &params, &in_buffer, &out_buffer, nullptr);
        const float* g = early_gain_buf_.data();
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* x = early_out_[c];
            float* sum = bus_[c];
            for (uint32_t j = 0; j < frames_; ++j) {
                sum[j] += x[j] * g[j];
            }
        }
        bus_used_ = true;
        if (input) {
            slot.early_tail = early_blocks_;
        } else if (slot.early_tail > 0) {
            --slot.early_tail;
        }
    }

    std::fill(late_storage_.begin(), late_storage_.end(), 0.0f);
    const uint32_t predelay = static_cast<uint32_t>(std::max(0, slot.delay));
    slot.late_sounding = slot.late.process(in, frames_, slot.reverb_times, slot.eq, predelay, late_out_.data());
    if (slot.late_sounding) {
        const float* tail = tail_gain_buf_.data();
        for (int k = 0; k < dsp::LateReverb::kOutputs; ++k) {
            const float* x = late_out_[static_cast<std::size_t>(k)];
            const auto& sh = late_sh_[static_cast<std::size_t>(k)];
            for (uint32_t c = 0; c < channels_; ++c) {
                const float g = sh[c];
                float* sum = bus_[c];
                for (uint32_t j = 0; j < frames_; ++j) {
                    sum[j] += g * tail[j] * x[j];
                }
            }
        }
        bus_used_ = true;
    }
}

bool ReflectionRenderer::render(const float* gain) noexcept {
    if (simulator_ == nullptr) {
        return false;
    }
    if (bus_used_) {
        std::fill(bus_storage_.begin(), bus_storage_.end(), 0.0f);
        bus_used_ = false;
    }
    early_gain_.render(early_gain_buf_.data(), frames_);
    tail_gain_.render(tail_gain_buf_.data(), frames_);
    uint32_t live = 0;
    uint32_t waiting = 0;
    uint32_t draining = 0;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        switch (slot.state) {
            case State::Free: break;
            case State::Waiting:
            case State::Live:
                if (take_results(i, slot)) {
                    slot.state = State::Live;
                }
                if (slot.state == State::Waiting) {
                    ++waiting;  // its voice is heard through the listener's reverb meanwhile
                    break;
                }
                if (slot.sent || slot.early_tail > 0 || slot.late_sounding) {
                    render_slot(slot, slot.sent, false);
                }
                live += i > 0 ? 1u : 0u;  // voices' own, not the listener's
                break;
            case State::Draining: {
                // Nothing more for the old voice can arrive once a simulation tick has started
                // without it; one more pass through the effect then takes whatever is in flight.
                const bool acknowledged = channel_->output(i).observed.load(std::memory_order_acquire) == 0;
                const bool flush = acknowledged && !slot.flushed;
                if (slot.early_tail > 0 || slot.late_sounding || flush) {
                    render_slot(slot, false, flush);
                    slot.flushed = slot.flushed || flush;
                }
                if (slot.flushed && slot.early_tail == 0 && !slot.late_sounding) {
                    slot.state = State::Free;
                } else {
                    ++draining;
                }
                break;
            }
        }
    }
    meter_.live_slots.store(live, std::memory_order_relaxed);
    meter_.waiting_slots.store(waiting, std::memory_order_relaxed);
    meter_.draining_slots.store(draining, std::memory_order_relaxed);
    if (!bus_used_) {
        mean_square_ *= 1.0f - meter_alpha_;
        meter_.mean_square.store(mean_square_, std::memory_order_relaxed);
        return false;
    }
    for (uint32_t c = 0; c < channels_; ++c) {
        float* x = bus_[c];
        for (uint32_t j = 0; j < frames_; ++j) {
            x[j] *= gain[j];
        }
    }
    float sum = 0.0f;
    for (uint32_t j = 0; j < frames_; ++j) {
        sum += bus_[0][j] * bus_[0][j];
    }
    // W carries the pressure times Y00 (1 / sqrt(4 pi)); the meter reports pressure.
    constexpr float kFourPi = 12.566370614f;
    mean_square_ += meter_alpha_ * (sum * kFourPi / static_cast<float>(frames_) - mean_square_);
    meter_.mean_square.store(mean_square_, std::memory_order_relaxed);
    return true;
}

void ReflectionRenderer::decode_speakers(const float right[3], const float up[3], const float ahead[3],
                                         float* const* out) noexcept {
    if (decoder_) {
        decoder_->decode(right, up, ahead, bus_.data(), out, frames_);
    }
}

}  // namespace vsa
