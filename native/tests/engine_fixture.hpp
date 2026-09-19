#pragma once

#include "vsaudio.h"

#include <doctest/doctest.h>

#include <mutex>
#include <string>
#include <vector>

namespace vsa_test {

struct CapturedLog {
    std::mutex mutex;
    std::vector<std::pair<vsa_log_level, std::string>> lines;

    static void VSA_CALL sink(void* user, vsa_log_level level, const char* message) {
        auto* self = static_cast<CapturedLog*>(user);
        std::lock_guard lock(self->mutex);
        self->lines.emplace_back(level, message);
    }

    bool contains(const std::string& needle) {
        std::lock_guard lock(mutex);
        for (const auto& [level, line] : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

inline vsa_engine_config make_config(vsa_ray_tracer ray_tracer, CapturedLog* log = nullptr) {
    vsa_engine_config config{};
    config.struct_size = sizeof config;
    config.abi_version = VSA_ABI_VERSION;
    config.log = log != nullptr ? &CapturedLog::sink : nullptr;
    config.log_user_data = log;
    config.ray_tracer = static_cast<uint32_t>(ray_tracer);
    return config;
}

/// RAII engine for tests.
struct ScopedEngine {
    vsa_engine* engine = nullptr;
    vsa_result result = VSA_ERROR_INTERNAL;

    explicit ScopedEngine(const vsa_engine_config& config) { result = vsa_engine_create(&config, &engine); }
    ~ScopedEngine() { vsa_engine_destroy(engine); }
    ScopedEngine(const ScopedEngine&) = delete;
    ScopedEngine& operator=(const ScopedEngine&) = delete;
};

}  // namespace vsa_test
