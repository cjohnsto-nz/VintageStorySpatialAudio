// C boundary. Every exported function:
//   - validates struct_size / ABI version before touching a struct,
//   - clears the thread's last error on entry,
//   - converts every exception into a vsa_result + last-error message.
// Nothing may throw past this file.

#include "vsaudio.h"

#include "audio/asset.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "engine.hpp"
#include "vsaudio_build_info.h"

#include <phonon_version.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

// The managed side marshals these enums as 32-bit integers.
static_assert(sizeof(vsa_result) == 4 && sizeof(vsa_log_level) == 4, "vsaudio enums must be 32-bit");
static_assert(std::is_standard_layout_v<vsa_engine_config> && std::is_standard_layout_v<vsa_self_test_report>);
// Layouts the managed bindings mirror (tests/.../NativeLayoutTests.cs). 64-bit targets only.
static_assert(sizeof(vsa_engine_config) == 56);
static_assert(sizeof(vsa_asset_desc) == 48 && offsetof(vsa_asset_desc, storage) == 32);
static_assert(sizeof(vsa_asset_info) == 40);
static_assert(sizeof(vsa_voice_desc) == 32 && offsetof(vsa_voice_desc, gain) == 16);
static_assert(sizeof(vsa_voice_status) == 16);
static_assert(sizeof(vsa_device_info) == 776 && offsetof(vsa_device_info, id) == 264);
static_assert(sizeof(vsa_output_desc) == 24);
static_assert(sizeof(vsa_engine_stats) == 352 && offsetof(vsa_engine_stats, blocks_rendered) == 40 &&
              offsetof(vsa_engine_stats, device_name) == 96);
static_assert(sizeof(vsa_event) == 32);

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

/// Input structs get the same checks as output structs.
template <typename T>
const T& check_in_struct(const T* in, const char* name) {
    check_out_struct(in, name);
    return *in;
}

/// For arrays of output records: the first element's struct_size stands for all of them.
template <typename T>
void check_out_array(T* out, uint32_t capacity, const char* name) {
    if (capacity > 0) {
        check_out_struct(out, name);
    }
}

vsa::Engine& engine_of(vsa_engine* engine) {
    if (engine == nullptr) {
        throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "engine must not be null");
    }
    return engine->engine;
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

// ---- assets ----

VSA_API vsa_result VSA_CALL vsa_asset_create(vsa_engine* engine, const vsa_asset_desc* desc, vsa_asset** out_asset) {
    return guarded([&] {
        if (out_asset == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_asset must not be null");
        }
        *out_asset = nullptr;
        const vsa_asset_desc& d = check_in_struct(desc, "vsa_asset_desc");
        *out_asset = reinterpret_cast<vsa_asset*>(engine_of(engine).create_asset(d));
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_asset_get_info(const vsa_asset* asset, vsa_asset_info* out) {
    return guarded([&] {
        if (asset == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "asset must not be null");
        }
        check_out_struct(out, "vsa_asset_info");
        *out = reinterpret_cast<const vsa::Asset*>(asset)->info();
        return VSA_OK;
    });
}

VSA_API void VSA_CALL vsa_asset_release(vsa_asset* asset) {
    if (asset != nullptr) {
        reinterpret_cast<vsa::Asset*>(asset)->release();
    }
}

// ---- voices ----

VSA_API vsa_result VSA_CALL vsa_voice_create(vsa_engine* engine, const vsa_voice_desc* desc, vsa_voice* out_voice) {
    return guarded([&] {
        if (out_voice == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_voice must not be null");
        }
        *out_voice = 0;
        const vsa_voice_desc& d = check_in_struct(desc, "vsa_voice_desc");
        *out_voice = engine_of(engine).create_voice(d);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_release(vsa_engine* engine, vsa_voice voice) {
    return guarded([&] {
        engine_of(engine).release_voice(voice);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_start(vsa_engine* engine, vsa_voice voice) {
    return guarded([&] {
        engine_of(engine).start_voice(voice);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_pause(vsa_engine* engine, vsa_voice voice) {
    return guarded([&] {
        engine_of(engine).pause_voice(voice);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_stop(vsa_engine* engine, vsa_voice voice) {
    return guarded([&] {
        engine_of(engine).stop_voice(voice);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_set_gain(vsa_engine* engine, vsa_voice voice, float gain) {
    return guarded([&] {
        engine_of(engine).set_voice_gain(voice, gain);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_set_pitch(vsa_engine* engine, vsa_voice voice, float pitch) {
    return guarded([&] {
        engine_of(engine).set_voice_pitch(voice, pitch);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_set_looping(vsa_engine* engine, vsa_voice voice, uint32_t looping) {
    return guarded([&] {
        engine_of(engine).set_voice_looping(voice, looping != 0);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_seek(vsa_engine* engine, vsa_voice voice, double position_seconds) {
    return guarded([&] {
        engine_of(engine).seek_voice(voice, position_seconds);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_fade(vsa_engine* engine, vsa_voice voice, float target_gain, float seconds,
                                           uint32_t flags, uint64_t token) {
    return guarded([&] {
        engine_of(engine).fade_voice(voice, target_gain, seconds, flags, token);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_get_status(vsa_engine* engine, vsa_voice voice, vsa_voice_status* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_voice_status");
        *out = engine_of(engine).voice_status(voice);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_bus_set_gain(vsa_engine* engine, uint32_t bus, float gain) {
    return guarded([&] {
        engine_of(engine).set_bus_gain(bus, gain);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_set_master_gain(vsa_engine* engine, float gain) {
    return guarded([&] {
        engine_of(engine).set_master_gain(gain);
        return VSA_OK;
    });
}

// ---- output ----

VSA_API vsa_result VSA_CALL vsa_device_enumerate(vsa_engine* engine, vsa_device_info* out, uint32_t capacity,
                                                 uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_count must not be null");
        }
        *out_count = 0;
        check_out_array(out, capacity, "vsa_device_info");
        const std::vector<vsa_device_info> devices = engine_of(engine).enumerate_devices();
        const std::size_t n = std::min<std::size_t>(capacity, devices.size());
        std::copy_n(devices.begin(), n, out);
        *out_count = static_cast<uint32_t>(devices.size());
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_output_open(vsa_engine* engine, const vsa_output_desc* desc) {
    return guarded([&] {
        const vsa_output_desc& d = check_in_struct(desc, "vsa_output_desc");
        engine_of(engine).open_output(d);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_render_offline(vsa_engine* engine, float* out, uint32_t frames) {
    return guarded([&] {
        engine_of(engine).render_offline(out, frames);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_get_stats(vsa_engine* engine, vsa_engine_stats* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_engine_stats");
        *out = engine_of(engine).stats();
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_poll_events(vsa_engine* engine, vsa_event* out, uint32_t capacity,
                                                   uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_count must not be null");
        }
        *out_count = 0;
        check_out_array(out, capacity, "vsa_event");
        *out_count = engine_of(engine).poll_events(out, capacity);
        return VSA_OK;
    });
}

VSA_API const char* VSA_CALL vsa_get_last_error(void) { return vsa::last_error(); }

}  // extern "C"
