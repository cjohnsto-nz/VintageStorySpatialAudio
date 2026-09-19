#include "audio/spatial.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "steam/steam_context.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace vsa {
namespace {

void check(IPLerror error, const char* what) {
    if (error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string(what) + " failed: " + steam::error_name(error));
    }
}

constexpr float kCentre = 0.70710678f;

// Order-3 binaural decoding of a point source loses energy away from the interaural axis (the
// virtual sources' HRTFs interfere at high frequencies): averaged over 26 directions around the
// head and 1-8 kHz it is 4.7 dB below the per-voice HRTF rendering (about 0 dB to the sides,
// 5-7 dB elsewhere). This restores the diffuse-field level, so a voice keeps its loudness when
// it moves between the binaural and ambisonic tiers. Measured by test_spatial.cpp.
constexpr float kAmbisonicMakeup = 1.72f;  // +4.7 dB

}  // namespace

SpatialRenderer::SpatialRenderer(const steam::SteamContext& steam, uint32_t pool_size)
    : steam_(steam), pool_size_(pool_size) {}

void SpatialRenderer::prepare(uint32_t sample_rate, uint32_t block_frames, uint32_t channels) {
    const auto started = std::chrono::steady_clock::now();

    // Release everything before recreating (Steam Audio objects are reference counted).
    sets_.clear();
    hrtf_.reset();

    IPLAudioSettings audio{};
    audio.samplingRate = static_cast<IPLint32>(sample_rate);
    audio.frameSize = static_cast<IPLint32>(block_frames);

    IPLHRTFSettings hrtf_settings{};
    hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
    hrtf_settings.volume = 1.0f;
    hrtf_settings.normType = IPL_HRTFNORMTYPE_NONE;
    check(iplHRTFCreate(steam_.context(), &audio, &hrtf_settings, hrtf_.out()), "iplHRTFCreate");

    IPLDirectEffectSettings direct{};
    direct.numChannels = 1;
    IPLBinauralEffectSettings binaural{};
    binaural.hrtf = hrtf_.get();
    IPLPanningEffectSettings panning{};
    vbap_.reset();
    switch (channels) {
        case 4: panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_QUADRAPHONIC; break;
        case 6: panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_5_1; break;
        case 8: panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1; break;
        case 12:
            // Heights: our own VBAP (the panning effect below is created stereo and not used).
            vbap_ = std::make_unique<Vbap>(channels);
            panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            break;
        default: panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO; break;
    }
    speaker_channels_ = is_supported_layout(channels) ? channels : 2;

    sets_.resize(pool_size_);
    for (EffectSet& set : sets_) {
        check(iplDirectEffectCreate(steam_.context(), &audio, &direct, set.direct.out()), "iplDirectEffectCreate");
        check(iplBinauralEffectCreate(steam_.context(), &audio, &binaural, set.binaural.out()), "iplBinauralEffectCreate");
        check(iplPanningEffectCreate(steam_.context(), &audio, &panning, set.panning.out()), "iplPanningEffectCreate");
    }

    // One binaural decoder for the whole world bus.
    IPLAmbisonicsDecodeEffectSettings decoder{};
    decoder.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    decoder.hrtf = hrtf_.get();
    decoder.maxOrder = kAmbisonicOrder;
    check(iplAmbisonicsDecodeEffectCreate(steam_.context(), &audio, &decoder, decoder_.out()),
          "iplAmbisonicsDecodeEffectCreate");
    bus_storage_.assign(static_cast<std::size_t>(block_frames) * kAmbisonicChannels, 0.0f);
    for (uint32_t c = 0; c < kAmbisonicChannels; ++c) {
        bus_[c] = bus_storage_.data() + static_cast<std::size_t>(c) * block_frames;
    }
    ramp_.resize(block_frames);
    for (uint32_t j = 0; j < block_frames; ++j) {
        ramp_[j] = static_cast<float>(j + 1) / static_cast<float>(block_frames);
    }
    bus_used_ = false;
    tail_blocks_ = 0;
    free_.resize(pool_size_);
    for (uint32_t i = 0; i < pool_size_; ++i) {
        free_[i] = static_cast<int>(pool_size_ - 1 - i);
    }
    free_count_ = pool_size_;
    frames_ = block_frames;

    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    Log::writef(VSA_LOG_INFO, "spatial: HRTF and %u effect sets ready for %u Hz / %u frames, %u-speaker panning (%.0f ms)",
                pool_size_, sample_rate, block_frames, speaker_channels_, ms);
}

int SpatialRenderer::acquire() noexcept {
    if (free_count_ == 0) {
        return -1;
    }
    return free_[--free_count_];
}

void SpatialRenderer::release(int set) noexcept {
    if (set >= 0 && free_count_ < pool_size_) {
        free_[free_count_++] = set;
    }
}

void SpatialRenderer::reset(int set) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    iplDirectEffectReset(s.direct.get());
    iplBinauralEffectReset(s.binaural.get());
    iplPanningEffectReset(s.panning.get());
    s.pan_ready = false;
    s.sh_ready = false;
}

void SpatialRenderer::reset_tier(int set, SpatialTier tier) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    switch (tier) {
        case SpatialTier::Binaural: iplBinauralEffectReset(s.binaural.get()); break;
        case SpatialTier::Ambisonic: s.sh_ready = false; break;
        case SpatialTier::Panned:
            iplPanningEffectReset(s.panning.get());
            s.pan_ready = false;
            break;
    }
}

void SpatialRenderer::reset_all() noexcept {
    for (std::size_t i = 0; i < sets_.size(); ++i) {
        reset(static_cast<int>(i));
    }
}

void SpatialRenderer::render(int set, vsa_render_mode mode, const SpatialParams& params, float* mono,
                             float* const* out) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    const auto frames = static_cast<IPLint32>(frames_);

    float* mono_channels[1] = {mono};
    IPLAudioBuffer mono_buffer{1, frames, mono_channels};
    float* out_channels[kMaxOutputChannels] = {};
    const uint32_t count = mode == VSA_RENDER_SPEAKERS ? speaker_channels_ : 2;
    for (uint32_t c = 0; c < count; ++c) {
        out_channels[c] = out[c];
    }
    IPLAudioBuffer out_buffer{static_cast<IPLint32>(count), frames, out_channels};

    apply_direct(s, params, mono);

    if (mode == VSA_RENDER_SPEAKERS && vbap_) {
        render_vbap(s, params, mono, out);
        return;
    }
    const IPLVector3 direction{params.direction[0], params.direction[1], params.direction[2]};
    if (mode == VSA_RENDER_SPEAKERS) {
        IPLPanningEffectParams panning{};
        panning.direction = direction;
        iplPanningEffectApply(s.panning.get(), &panning, &mono_buffer, &out_buffer);
        // At the head: blend towards the unpositioned rendering (front pair, -3 dB each).
        const float blend = params.spatial_blend;
        if (blend < 1.0f) {
            const float centre = (1.0f - blend) * kCentre;
            for (uint32_t c = 0; c < count; ++c) {
                float* x = out[c];
                for (IPLint32 j = 0; j < frames; ++j) {
                    x[j] = blend * x[j] + (c < 2 ? centre * mono[j] : 0.0f);
                }
            }
        }
        return;
    }

    IPLBinauralEffectParams binaural{};
    binaural.direction = direction;
    binaural.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    binaural.spatialBlend = params.spatial_blend;
    binaural.hrtf = hrtf_.get();
    iplBinauralEffectApply(s.binaural.get(), &binaural, &mono_buffer, &out_buffer);
}

void SpatialRenderer::render_vbap(EffectSet& set, const SpatialParams& params, const float* mono,
                                  float* const* out) noexcept {
    std::array<float, kMaxOutputChannels> target{};
    vbap_->gains(params.direction, target.data());
    // At the head: blend towards the unpositioned rendering (front pair, -3 dB each).
    const float blend = params.spatial_blend;
    for (uint32_t c = 0; c < speaker_channels_; ++c) {
        target[c] = blend * target[c] + (c < 2 ? (1.0f - blend) * kCentre : 0.0f);
    }
    if (!set.pan_ready) {
        set.pan = target;
        set.pan_ready = true;
    }
    // Gains ramp across the block, so moving sources and turning heads do not step.
    const float* ramp = ramp_.data();
    for (uint32_t c = 0; c < speaker_channels_; ++c) {
        const float from = set.pan[c];
        const float change = target[c] - from;
        float* x = out[c];
        for (uint32_t j = 0; j < frames_; ++j) {
            x[j] = (from + change * ramp[j]) * mono[j];
        }
    }
    set.pan = target;
}

void SpatialRenderer::apply_direct(EffectSet& set, const SpatialParams& params, float* mono) noexcept {
    float* channels[1] = {mono};
    IPLAudioBuffer buffer{1, static_cast<IPLint32>(frames_), channels};
    IPLDirectEffectParams direct{};
    direct.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION |
                                                     IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
    direct.distanceAttenuation = params.distance_gain;
    direct.airAbsorption[0] = params.air_absorption[0];
    direct.airAbsorption[1] = params.air_absorption[1];
    direct.airAbsorption[2] = params.air_absorption[2];
    iplDirectEffectApply(set.direct.get(), &direct, &buffer, &buffer);
}

void SpatialRenderer::begin_block() noexcept {
    if (bus_used_) {
        std::fill(bus_storage_.begin(), bus_storage_.end(), 0.0f);
        bus_used_ = false;
    }
}

void SpatialRenderer::encode(int set, const SpatialParams& params, float* mono) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    apply_direct(s, params, mono);

    // Encoded here rather than with Steam Audio's encode effect: the coefficients are ramped
    // across the block (a moving source does not step) and summed straight into the bus.
    std::array<float, kAmbisonicChannels> target;
    dsp::sh_order3(params.world_direction, target.data());
    const float blend = params.spatial_blend;
    for (uint32_t c = 1; c < kAmbisonicChannels; ++c) {
        target[c] *= blend;  // towards the head only the omnidirectional W remains: centred
    }
    if (!s.sh_ready) {
        s.sh = target;
        s.sh_ready = true;
    }
    const float* ramp = ramp_.data();
    for (uint32_t c = 0; c < kAmbisonicChannels; ++c) {
        const float from = s.sh[c];
        const float change = target[c] - from;
        float* sum = bus_[c];
        if (change == 0.0f) {
            for (uint32_t j = 0; j < frames_; ++j) {
                sum[j] += from * mono[j];
            }
        } else {
            for (uint32_t j = 0; j < frames_; ++j) {
                sum[j] += (from + change * ramp[j]) * mono[j];
            }
        }
    }
    s.sh = target;
    bus_used_ = true;
}

bool SpatialRenderer::decode(const Orientation& o, float* left, float* right) noexcept {
    constexpr uint32_t kTailBlocks = 16;  // generous: the HRTF convolution tail is a few hundred samples
    if (bus_used_) {
        tail_blocks_ = kTailBlocks;
    } else if (tail_blocks_ == 0) {
        return false;
    } else {
        --tail_blocks_;
    }

    const auto frames = static_cast<IPLint32>(frames_);
    IPLAudioBuffer in{static_cast<IPLint32>(kAmbisonicChannels), frames, bus_.data()};
    float* out_channels[2] = {left, right};
    IPLAudioBuffer out{2, frames, out_channels};
    IPLAmbisonicsDecodeEffectParams decode{};
    decode.order = kAmbisonicOrder;
    decode.hrtf = hrtf_.get();
    decode.orientation.right = IPLVector3{o.right[0], o.right[1], o.right[2]};
    decode.orientation.up = IPLVector3{o.up[0], o.up[1], o.up[2]};
    decode.orientation.ahead = IPLVector3{o.ahead[0], o.ahead[1], o.ahead[2]};
    decode.orientation.origin = IPLVector3{o.origin[0], o.origin[1], o.origin[2]};
    decode.binaural = IPL_TRUE;
    iplAmbisonicsDecodeEffectApply(decoder_.get(), &decode, &in, &out);
    for (uint32_t j = 0; j < frames_; ++j) {
        left[j] *= kAmbisonicMakeup;
        right[j] *= kAmbisonicMakeup;
    }
    return true;
}

}  // namespace vsa
