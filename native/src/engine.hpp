#pragma once

#include "steam/steam_context.hpp"
#include "vsaudio.h"

#include <memory>

namespace vsa {

/// Root object behind the opaque `vsa_engine` handle.
///
/// Phase 0 scope: Steam Audio context + ray tracer selection + self-test.
/// Later phases add the asset store, voices, simulation threads and output backends
/// here, each owned by the engine so destruction order is explicit.
class Engine {
public:
    explicit Engine(const vsa_engine_config& config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] vsa_engine_info info() const noexcept;
    [[nodiscard]] vsa_self_test_report run_self_test() const;

private:
    std::unique_ptr<steam::SteamContext> steam_;
};

}  // namespace vsa
