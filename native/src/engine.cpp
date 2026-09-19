#include "engine.hpp"

#include "core/log.hpp"
#include "steam/self_test.hpp"

namespace vsa {

Engine::Engine(const vsa_engine_config& config) {
    steam::SteamContext::Options options;
    options.ray_tracer = static_cast<vsa_ray_tracer>(config.ray_tracer);  // validated in api.cpp
    options.validation = (config.flags & VSA_ENGINE_FLAG_STEAM_AUDIO_VALIDATION) != 0;
    steam_ = std::make_unique<steam::SteamContext>(options);
}

Engine::~Engine() {
    steam_.reset();
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

}  // namespace vsa
