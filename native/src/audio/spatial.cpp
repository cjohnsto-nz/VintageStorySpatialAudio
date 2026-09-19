#include "audio/spatial.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "steam/steam_context.hpp"

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

}  // namespace

SpatialRenderer::SpatialRenderer(const steam::SteamContext& steam, uint32_t pool_size)
    : steam_(steam), pool_size_(pool_size) {}

void SpatialRenderer::prepare(uint32_t sample_rate, uint32_t block_frames) {
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
    panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;

    sets_.resize(pool_size_);
    for (EffectSet& set : sets_) {
        check(iplDirectEffectCreate(steam_.context(), &audio, &direct, set.direct.out()), "iplDirectEffectCreate");
        check(iplBinauralEffectCreate(steam_.context(), &audio, &binaural, set.binaural.out()), "iplBinauralEffectCreate");
        check(iplPanningEffectCreate(steam_.context(), &audio, &panning, set.panning.out()), "iplPanningEffectCreate");
    }
    free_.resize(pool_size_);
    for (uint32_t i = 0; i < pool_size_; ++i) {
        free_[i] = static_cast<int>(pool_size_ - 1 - i);
    }
    free_count_ = pool_size_;
    frames_ = block_frames;

    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    Log::writef(VSA_LOG_INFO, "spatial: HRTF and %u effect sets ready for %u Hz / %u frames (%.0f ms)", pool_size_,
                sample_rate, block_frames, ms);
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
}

void SpatialRenderer::reset_spatialiser(int set, bool binaural) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    if (binaural) {
        iplBinauralEffectReset(s.binaural.get());
    } else {
        iplPanningEffectReset(s.panning.get());
    }
}

void SpatialRenderer::reset_all() noexcept {
    for (std::size_t i = 0; i < sets_.size(); ++i) {
        reset(static_cast<int>(i));
    }
}

void SpatialRenderer::render(int set, vsa_render_mode mode, const SpatialParams& params, float* mono, float* out_left,
                             float* out_right) noexcept {
    EffectSet& s = sets_[static_cast<std::size_t>(set)];
    const auto frames = static_cast<IPLint32>(frames_);

    float* mono_channels[1] = {mono};
    IPLAudioBuffer mono_buffer{1, frames, mono_channels};
    float* stereo_channels[2] = {out_left, out_right};
    IPLAudioBuffer stereo_buffer{2, frames, stereo_channels};

    IPLDirectEffectParams direct{};
    direct.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION |
                                                     IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
    direct.distanceAttenuation = params.distance_gain;
    direct.airAbsorption[0] = params.air_absorption[0];
    direct.airAbsorption[1] = params.air_absorption[1];
    direct.airAbsorption[2] = params.air_absorption[2];
    iplDirectEffectApply(s.direct.get(), &direct, &mono_buffer, &mono_buffer);

    const IPLVector3 direction{params.direction[0], params.direction[1], params.direction[2]};
    if (mode == VSA_RENDER_SPEAKERS) {
        IPLPanningEffectParams panning{};
        panning.direction = direction;
        iplPanningEffectApply(s.panning.get(), &panning, &mono_buffer, &stereo_buffer);
        const float blend = params.spatial_blend;
        if (blend < 1.0f) {
            const float centre = (1.0f - blend) * kCentre;
            for (IPLint32 j = 0; j < frames; ++j) {
                out_left[j] = blend * out_left[j] + centre * mono[j];
                out_right[j] = blend * out_right[j] + centre * mono[j];
            }
        }
        return;
    }

    IPLBinauralEffectParams binaural{};
    binaural.direction = direction;
    binaural.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    binaural.spatialBlend = params.spatial_blend;
    binaural.hrtf = hrtf_.get();
    iplBinauralEffectApply(s.binaural.get(), &binaural, &mono_buffer, &stereo_buffer);
}

}  // namespace vsa
