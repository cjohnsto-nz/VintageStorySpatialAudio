#pragma once

#include "steam/ipl_handle.hpp"
#include "vsaudio.h"

namespace vsa::steam {

/// Owns the Steam Audio context and the ray tracer choice for the process.
class SteamContext {
public:
    struct Options {
        vsa_ray_tracer ray_tracer = VSA_RAY_TRACER_AUTO;
        bool validation = false;
    };

    explicit SteamContext(const Options& options);

    /// Whether Embree can be used in this process (Steam Audio has none for Apple Silicon).
    /// Asked once, quietly, and remembered: defaults that depend on the ray tracer are resolved
    /// before any engine, and so before any context, exists.
    [[nodiscard]] static bool embree_on_this_machine() noexcept;

    SteamContext(const SteamContext&) = delete;
    SteamContext& operator=(const SteamContext&) = delete;

    [[nodiscard]] IPLContext context() const noexcept { return context_.get(); }
    [[nodiscard]] IPLSceneType scene_type() const noexcept { return scene_type_; }
    /// Embree device, or nullptr when using Steam Audio's built-in tracer.
    [[nodiscard]] IPLEmbreeDevice embree_device() const noexcept { return embree_.get(); }
    [[nodiscard]] vsa_ray_tracer active_ray_tracer() const noexcept;
    [[nodiscard]] bool embree_available() const noexcept { return embree_available_; }

    /// Scene settings pre-filled for the active ray tracer.
    [[nodiscard]] IPLSceneSettings scene_settings() const noexcept;

private:
    Context context_;
    EmbreeDevice embree_;
    IPLSceneType scene_type_ = IPL_SCENETYPE_DEFAULT;
    bool embree_available_ = false;
};

}  // namespace vsa::steam
