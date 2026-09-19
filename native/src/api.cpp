// C boundary. Every exported function:
//   - validates struct_size / ABI version before touching a struct,
//   - clears the thread's last error on entry,
//   - converts every exception into a vsa_result + last-error message.
// Nothing may throw past this file.

#include "vsaudio.h"

#include "core/error.hpp"
#include "core/log.hpp"
#include "engine.hpp"
#include "vsaudio_build_info.h"

#include <phonon_version.h>

#include <atomic>
#include <new>
#include <type_traits>

// The managed side marshals these enums as 32-bit integers.
static_assert(sizeof(vsa_result) == 4 && sizeof(vsa_log_level) == 4, "vsaudio enums must be 32-bit");
static_assert(std::is_standard_layout_v<vsa_engine_config> && std::is_standard_layout_v<vsa_self_test_report>);

struct vsa_engine {
    vsa::Engine engine;
    explicit vsa_engine(const vsa_engine_config& config) : engine(config) {}
};

namespace {

std::atomic<bool> g_engine_exists{false};

template <typename Fn>
vsa_result guarded(Fn&& fn) noexcept {
    vsa::clear_last_error();
    try {
        return fn();
    } catch (const vsa::Error& e) {
        vsa::set_last_error(e.what());
        return e.code();
    } catch (const std::bad_alloc&) {
        vsa::set_last_error("out of memory");
        return VSA_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        vsa::set_last_error(e.what());
        return VSA_ERROR_INTERNAL;
    } catch (...) {
        vsa::set_last_error("unknown internal error");
        return VSA_ERROR_INTERNAL;
    }
}

template <typename T>
vsa_result check_out_struct(T* out, const char* name) {
    if (out == nullptr) {
        throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, std::string(name) + " must not be null");
    }
    if (out->struct_size != sizeof(T)) {
        throw vsa::Error(VSA_ERROR_ABI_MISMATCH, std::string(name) + ".struct_size is " +
                                                     std::to_string(out->struct_size) + ", expected " +
                                                     std::to_string(sizeof(T)));
    }
    return VSA_OK;
}

}  // namespace

extern "C" {

VSA_API vsa_result VSA_CALL vsa_get_version(vsa_version_info* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_version_info");
        out->abi_version = VSA_ABI_VERSION;
        out->engine_major = VSA_VERSION_MAJOR;
        out->engine_minor = VSA_VERSION_MINOR;
        out->engine_patch = VSA_VERSION_PATCH;
        out->steam_audio_major = STEAMAUDIO_VERSION_MAJOR;
        out->steam_audio_minor = STEAMAUDIO_VERSION_MINOR;
        out->steam_audio_patch = STEAMAUDIO_VERSION_PATCH;
        out->build_description = VSA_BUILD_DESCRIPTION " (" VSA_BUILD_CONFIG ")";
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_create(const vsa_engine_config* config, vsa_engine** out_engine) {
    return guarded([&] {
        if (out_engine == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_engine must not be null");
        }
        *out_engine = nullptr;
        if (config == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "config must not be null");
        }
        if (config->struct_size != sizeof(vsa_engine_config) || config->abi_version != VSA_ABI_VERSION) {
            throw vsa::Error(VSA_ERROR_ABI_MISMATCH,
                             "vsa_engine_config mismatch: caller struct_size " + std::to_string(config->struct_size) +
                                 " / abi " + std::to_string(config->abi_version) + ", engine expects " +
                                 std::to_string(sizeof(vsa_engine_config)) + " / abi " +
                                 std::to_string(VSA_ABI_VERSION));
        }
        if (config->ray_tracer != VSA_RAY_TRACER_AUTO && config->ray_tracer != VSA_RAY_TRACER_EMBREE &&
            config->ray_tracer != VSA_RAY_TRACER_STEAM) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT,
                             "unknown ray_tracer value " + std::to_string(config->ray_tracer));
        }

        bool expected = false;
        if (!g_engine_exists.compare_exchange_strong(expected, true)) {
            throw vsa::Error(VSA_ERROR_ALREADY_EXISTS, "an engine already exists in this process");
        }

        vsa::Log::set_sink(config->log, config->log_user_data);
        try {
            *out_engine = new vsa_engine(*config);
        } catch (...) {
            vsa::Log::clear_sink();
            g_engine_exists.store(false);
            throw;
        }
        vsa::Log::writef(VSA_LOG_INFO, "vsaudio %d.%d.%d created [%s, %s]", VSA_VERSION_MAJOR, VSA_VERSION_MINOR,
                         VSA_VERSION_PATCH, VSA_BUILD_DESCRIPTION, VSA_BUILD_CONFIG);
        return VSA_OK;
    });
}

VSA_API void VSA_CALL vsa_engine_destroy(vsa_engine* engine) {
    if (engine == nullptr) {
        return;
    }
    // Destructors are noexcept; Steam Audio release functions do not throw.
    delete engine;
    vsa::Log::clear_sink();
    g_engine_exists.store(false);
}

VSA_API vsa_result VSA_CALL vsa_engine_get_info(const vsa_engine* engine, vsa_engine_info* out) {
    return guarded([&] {
        if (engine == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "engine must not be null");
        }
        check_out_struct(out, "vsa_engine_info");
        *out = engine->engine.info();
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_run_self_test(vsa_engine* engine, vsa_self_test_report* out) {
    return guarded([&] {
        if (engine == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "engine must not be null");
        }
        check_out_struct(out, "vsa_self_test_report");
        *out = engine->engine.run_self_test();
        return VSA_OK;
    });
}

VSA_API const char* VSA_CALL vsa_get_last_error(void) { return vsa::last_error(); }

}  // extern "C"
