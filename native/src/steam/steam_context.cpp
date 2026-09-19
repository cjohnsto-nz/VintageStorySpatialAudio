#include "steam/steam_context.hpp"

#include "core/error.hpp"
#include "core/log.hpp"

#include <string>

namespace vsa::steam {
namespace {

void IPLCALL on_steam_audio_log(IPLLogLevel level, const char* message) {
    vsa_log_level mapped = VSA_LOG_INFO;
    switch (level) {
        case IPL_LOGLEVEL_DEBUG: mapped = VSA_LOG_DEBUG; break;
        case IPL_LOGLEVEL_INFO: mapped = VSA_LOG_INFO; break;
        case IPL_LOGLEVEL_WARNING: mapped = VSA_LOG_WARNING; break;
        case IPL_LOGLEVEL_ERROR: mapped = VSA_LOG_ERROR; break;
    }
    Log::writef(mapped, "[steamaudio] %s", message != nullptr ? message : "");
}

// Upper bound for Steam Audio's SIMD dispatch; it picks the best level the CPU
// supports at or below this. (AVX-512 throttling is a known issue on some CPUs;
// this becomes a user setting in Phase 8.)
constexpr IPLSIMDLevel max_simd_level() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return IPL_SIMDLEVEL_NEON;
#else
    return IPL_SIMDLEVEL_AVX512;
#endif
}

}  // namespace

const char* error_name(IPLerror error) noexcept {
    switch (error) {
        case IPL_STATUS_SUCCESS: return "IPL_STATUS_SUCCESS";
        case IPL_STATUS_FAILURE: return "IPL_STATUS_FAILURE";
        case IPL_STATUS_OUTOFMEMORY: return "IPL_STATUS_OUTOFMEMORY";
        case IPL_STATUS_INITIALIZATION: return "IPL_STATUS_INITIALIZATION";
    }
    return "IPL_STATUS_<unknown>";
}

SteamContext::SteamContext(const Options& options) {
    IPLContextSettings settings{};
    settings.version = STEAMAUDIO_VERSION;
    settings.logCallback = &on_steam_audio_log;
    settings.simdLevel = max_simd_level();
    settings.flags = options.validation ? IPL_CONTEXTFLAGS_VALIDATION : static_cast<IPLContextFlags>(0);

    if (const IPLerror error = iplContextCreate(&settings, context_.out()); error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string("iplContextCreate failed: ") + error_name(error));
    }

    if (options.ray_tracer != VSA_RAY_TRACER_STEAM) {
        IPLEmbreeDeviceSettings embree_settings{};
        const IPLerror error = iplEmbreeDeviceCreate(context_.get(), &embree_settings, embree_.out());
        embree_available_ = error == IPL_STATUS_SUCCESS && embree_;
        if (!embree_available_) {
            embree_.reset();
            if (options.ray_tracer == VSA_RAY_TRACER_EMBREE) {
                throw Error(VSA_ERROR_UNSUPPORTED,
                            std::string("Embree was requested but iplEmbreeDeviceCreate failed: ") + error_name(error));
            }
            Log::writef(VSA_LOG_INFO, "Embree unavailable (%s); using Steam Audio's built-in ray tracer", error_name(error));
        }
    }

    scene_type_ = embree_ ? IPL_SCENETYPE_EMBREE : IPL_SCENETYPE_DEFAULT;
    Log::writef(VSA_LOG_INFO, "Steam Audio %u.%u.%u context created; ray tracer: %s", STEAMAUDIO_VERSION_MAJOR,
                STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, embree_ ? "Embree" : "Steam Audio built-in");
}

vsa_ray_tracer SteamContext::active_ray_tracer() const noexcept {
    return scene_type_ == IPL_SCENETYPE_EMBREE ? VSA_RAY_TRACER_EMBREE : VSA_RAY_TRACER_STEAM;
}

IPLSceneSettings SteamContext::scene_settings() const noexcept {
    IPLSceneSettings settings{};
    settings.type = scene_type_;
    settings.embreeDevice = embree_.get();
    return settings;
}

}  // namespace vsa::steam
