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
#include "world/world_scene.hpp"
#include "vsaudio_build_info.h"

#include <phonon_version.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <new>
#include <type_traits>

// The managed side marshals these enums as 32-bit integers.
static_assert(sizeof(vsa_result) == 4 && sizeof(vsa_log_level) == 4, "vsaudio enums must be 32-bit");
static_assert(std::is_standard_layout_v<vsa_engine_config> && std::is_standard_layout_v<vsa_self_test_report>);
// Layouts the managed bindings mirror (tests/.../NativeLayoutTests.cs). 64-bit targets only.
static_assert(sizeof(vsa_engine_config) == 112 && offsetof(vsa_engine_config, max_binaural_voices) == 56 &&
              offsetof(vsa_engine_config, hrtf_sofa_path) == 64 && offsetof(vsa_engine_config, direct_rate_hz) == 76 &&
              offsetof(vsa_engine_config, reflection_sources) == 80 &&
              offsetof(vsa_engine_config, reflection_transition) == 108);
static_assert(sizeof(vsa_reflection_stats) == 120 && offsetof(vsa_reflection_stats, ticks) == 56 &&
              offsetof(vsa_reflection_stats, listener_reverb_times) == 88 && offsetof(vsa_reflection_stats, listener) == 108);
static_assert(sizeof(vsa_reflection_source) == 56 && offsetof(vsa_reflection_source, voice) == 8 &&
              offsetof(vsa_reflection_source, delay) == 52);
static_assert(sizeof(vsa_ray_segment) == 40 && offsetof(vsa_ray_segment, energy) == 32);
static_assert(sizeof(vsa_source_debug) == 72 && offsetof(vsa_source_debug, position) == 16 &&
              offsetof(vsa_source_debug, crossings) == 60 && offsetof(vsa_source_debug, air_path) == 64);
static_assert(sizeof(vsa_simulation_stats) == 80 && offsetof(vsa_simulation_stats, rate_hz) == 48 &&
              offsetof(vsa_simulation_stats, origin) == 68);
static_assert(sizeof(vsa_asset_desc) == 48 && offsetof(vsa_asset_desc, storage) == 32);
static_assert(sizeof(vsa_asset_info) == 40);
static_assert(sizeof(vsa_voice_desc) == 48 && offsetof(vsa_voice_desc, gain) == 16 && offsetof(vsa_voice_desc, position) == 32);
static_assert(sizeof(vsa_voice_status) == 16);
static_assert(sizeof(vsa_device_info) == 776 && offsetof(vsa_device_info, id) == 264);
static_assert(sizeof(vsa_output_desc) == 24);
static_assert(sizeof(vsa_engine_stats) == 360 && offsetof(vsa_engine_stats, real_voices) == 40 &&
              offsetof(vsa_engine_stats, blocks_rendered) == 48 && offsetof(vsa_engine_stats, device_name) == 104);
static_assert(sizeof(vsa_listener) == 52 && offsetof(vsa_listener, render_offset) == 40);
static_assert(sizeof(vsa_acoustic_material) == 56 && offsetof(vsa_acoustic_material, name) == 48);
static_assert(sizeof(vsa_box) == 24 && sizeof(vsa_partial_block) == 16);
static_assert(sizeof(vsa_chunk_desc) == 56 && offsetof(vsa_chunk_desc, materials) == 24 &&
              offsetof(vsa_chunk_desc, boxes) == 48);
static_assert(sizeof(vsa_scene_stats) == 88 && offsetof(vsa_scene_stats, last_build_ms) == 48 &&
              offsetof(vsa_scene_stats, origin) == 72);
static_assert(sizeof(vsa_chunk_mesh) == 56 && offsetof(vsa_chunk_mesh, vertices) == 32);
static_assert(sizeof(vsa_ray_hit) == 76 && offsetof(vsa_ray_hit, triangle) == 48 && offsetof(vsa_ray_hit, cell) == 60);
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

VSA_API vsa_result VSA_CALL vsa_voice_set_position(vsa_engine* engine, vsa_voice voice, uint32_t spatial, float x,
                                                   float y, float z) {
    return guarded([&] {
        engine_of(engine).set_voice_position(voice, spatial, x, y, z);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_voice_set_lowpass(vsa_engine* engine, vsa_voice voice, float gain_hf) {
    return guarded([&] {
        engine_of(engine).set_voice_lowpass(voice, gain_hf);
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

VSA_API vsa_result VSA_CALL vsa_listener_set(vsa_engine* engine, const vsa_listener* listener) {
    return guarded([&] {
        const vsa_listener& l = check_in_struct(listener, "vsa_listener");
        engine_of(engine).set_listener(l);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_set_render_mode(vsa_engine* engine, uint32_t mode) {
    return guarded([&] {
        engine_of(engine).set_render_mode(mode);
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

// ---- World scene ----

VSA_API vsa_result VSA_CALL vsa_scene_set_materials(vsa_engine* engine, const vsa_acoustic_material* materials,
                                                    uint32_t count) {
    return guarded([&] {
        vsa::world::WorldScene& scene = engine_of(engine).scene();
        if (count == 0 || count > 65535 || materials == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "the material table needs 1..65535 entries");
        }
        std::vector<vsa::world::AcousticMaterial> table;
        table.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const vsa_acoustic_material& in = check_in_struct(materials + i, "vsa_acoustic_material");
            if (in.kind > VSA_MATERIAL_SOLID) {
                throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "material " + std::to_string(i) + ": unknown kind");
            }
            if (i == 0 && in.kind != VSA_MATERIAL_AIR) {
                throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "material 0 must be air");
            }
            vsa::world::AcousticMaterial m;
            m.name = in.name != nullptr ? in.name : "";
            m.kind = static_cast<vsa::world::MaterialKind>(in.kind);
            std::copy_n(in.absorption, 3, m.absorption);
            m.scattering = in.scattering;
            std::copy_n(in.transmission, 3, m.transmission);
            std::copy_n(in.attenuation_db_per_metre, 3, m.attenuation_db_per_metre);
            table.push_back(std::move(m));
        }
        scene.set_materials(std::move(table));
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_set_chunk(vsa_engine* engine, const vsa_chunk_desc* chunk) {
    return guarded([&] {
        vsa::world::WorldScene& scene = engine_of(engine).scene();
        const vsa_chunk_desc& desc = check_in_struct(chunk, "vsa_chunk_desc");
        if (desc.materials == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "materials must not be null");
        }
        if (desc.lod > 1 || desc.reserved != 0) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "lod must be 0 or 1 and reserved 0");
        }
        if ((desc.partial_count > 0 && desc.partials == nullptr) || (desc.box_count > 0 && desc.boxes == nullptr)) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "partials/boxes must not be null when counted");
        }
        const auto material_count = static_cast<uint32_t>(scene.material_count());
        auto voxels = std::make_shared<vsa::world::ChunkVoxels>();
        std::copy_n(desc.materials, VSA_CHUNK_CELLS, voxels->materials.begin());
        const uint16_t highest = *std::max_element(voxels->materials.begin(), voxels->materials.end());
        if (highest >= material_count) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "material id " + std::to_string(highest) +
                                                             " is outside the material table (" +
                                                             std::to_string(material_count) + " entries)");
        }
        voxels->partials.reserve(desc.partial_count);
        for (uint32_t i = 0; i < desc.partial_count; ++i) {
            const vsa_partial_block& p = desc.partials[i];
            if (p.cell >= VSA_CHUNK_CELLS || p.material >= material_count ||
                static_cast<uint64_t>(p.first_box) + p.box_count > desc.box_count) {
                throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "partial block " + std::to_string(i) + " is out of range");
            }
            vsa::world::PartialBlock block;
            block.cell = static_cast<uint16_t>(p.cell);
            block.material = static_cast<uint16_t>(p.material);
            for (uint32_t b = 0; b < p.box_count; ++b) {
                const vsa_box& box = desc.boxes[p.first_box + b];
                vsa::world::Box out{};
                std::copy_n(box.min, 3, out.min);
                std::copy_n(box.max, 3, out.max);
                block.boxes.push_back(out);
            }
            voxels->partials.push_back(std::move(block));
        }
        vsa::world::sort_partials(*voxels);
        scene.set_chunk({desc.x, desc.y, desc.z}, std::move(voxels), static_cast<int>(desc.lod));
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_remove_chunk(vsa_engine* engine, int32_t x, int32_t y, int32_t z) {
    return guarded([&] {
        engine_of(engine).scene().remove_chunk({x, y, z});
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_clear(vsa_engine* engine) {
    return guarded([&] {
        engine_of(engine).scene().clear();
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_set_origin(vsa_engine* engine, int32_t x, int32_t y, int32_t z) {
    return guarded([&] {
        engine_of(engine).scene().set_origin(x, y, z);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_wait_idle(vsa_engine* engine, uint32_t timeout_ms) {
    return guarded([&] {
        if (!engine_of(engine).scene().wait_idle(std::chrono::milliseconds(timeout_ms))) {
            throw vsa::Error(VSA_ERROR_INVALID_STATE, "the scene is still busy");
        }
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_get_stats(vsa_engine* engine, vsa_scene_stats* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_scene_stats");
        const vsa::world::WorldScene& scene = engine_of(engine).scene();
        const vsa::world::SceneStats s = scene.stats();
        vsa_scene_stats stats{};
        stats.struct_size = sizeof stats;
        stats.chunks = s.chunks;
        stats.meshed_chunks = s.meshed_chunks;
        stats.pending_chunks = s.pending_chunks;
        stats.triangles = s.triangles;
        stats.vertices = s.vertices;
        stats.memory_bytes = s.memory_bytes;
        stats.chunks_built = s.chunks_built;
        stats.last_build_ms = s.last_build_ms;
        stats.max_build_ms = s.max_build_ms;
        stats.last_commit_ms = s.last_commit_ms;
        std::copy_n(s.origin, 3, stats.origin);
        stats.material_count = static_cast<uint32_t>(scene.material_count());
        *out = stats;
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_get_chunk_mesh(vsa_engine* engine, int32_t x, int32_t y, int32_t z,
                                                     vsa_chunk_mesh* mesh) {
    return guarded([&] {
        check_out_struct(mesh, "vsa_chunk_mesh");
        std::shared_ptr<const vsa::world::ChunkMesh> found;
        int lod = 0;
        uint32_t version = 0;
        mesh->found = 0;
        mesh->lod = 0;
        mesh->version = 0;
        mesh->vertex_count = 0;
        mesh->triangle_count = 0;
        if (!engine_of(engine).scene().chunk_mesh({x, y, z}, found, lod, version)) {
            return VSA_OK;
        }
        mesh->found = 1;
        mesh->version = version;
        mesh->lod = static_cast<uint32_t>(lod);
        mesh->vertex_count = static_cast<uint32_t>(found->vertex_count());
        mesh->triangle_count = static_cast<uint32_t>(found->triangle_count());
        if (mesh->vertex_capacity >= mesh->vertex_count && mesh->triangle_capacity >= mesh->triangle_count) {
            if ((mesh->vertex_count > 0 && mesh->vertices == nullptr) ||
                (mesh->triangle_count > 0 && (mesh->triangles == nullptr || mesh->materials == nullptr))) {
                throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "mesh buffers must not be null");
            }
            std::copy(found->vertices.begin(), found->vertices.end(), mesh->vertices);
            std::copy(found->triangles.begin(), found->triangles.end(), mesh->triangles);
            std::copy(found->materials.begin(), found->materials.end(), mesh->materials);
        }
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_list_chunks(vsa_engine* engine, int32_t* out, uint32_t capacity,
                                                  uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr || (capacity > 0 && out == nullptr)) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_count (and out, with a capacity) must not be null");
        }
        const std::vector<vsa::world::ChunkKey> keys = engine_of(engine).scene().chunk_keys();
        *out_count = static_cast<uint32_t>(keys.size());
        for (std::size_t i = 0; i < capacity && i < keys.size(); ++i) {
            out[i * 3] = keys[i].x;
            out[i * 3 + 1] = keys[i].y;
            out[i * 3 + 2] = keys[i].z;
        }
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_raycast(vsa_engine* engine, const float origin[3], const float direction[3],
                                              float max_distance, vsa_ray_hit* hit) {
    return guarded([&] {
        check_out_struct(hit, "vsa_ray_hit");
        if (origin == nullptr || direction == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "origin and direction must not be null");
        }
        vsa::world::RayHit found;
        const bool any = engine_of(engine).scene().raycast(origin, direction, max_distance, found);
        vsa_ray_hit out{};
        out.struct_size = sizeof out;
        out.hit = any ? 1u : 0u;
        if (any) {
            out.distance = found.distance;
            std::copy_n(found.point, 3, out.point);
            std::copy_n(found.normal, 3, out.normal);
            out.chunk[0] = found.chunk.x;
            out.chunk[1] = found.chunk.y;
            out.chunk[2] = found.chunk.z;
            out.triangle = found.triangle;
            out.material = found.material;
            out.from_partial = found.from_partial ? 1u : 0u;
            std::copy_n(found.cell, 3, out.cell);
            out.lod = static_cast<uint32_t>(found.lod);
        }
        *hit = out;
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_save_obj(vsa_engine* engine, const char* path) {
    return guarded([&] {
        if (path == nullptr || path[0] == 0) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "path must not be empty");
        }
        engine_of(engine).scene().save_obj(path);
        return VSA_OK;
    });
}

// ---- Direct simulation ----

VSA_API vsa_result VSA_CALL vsa_engine_get_sources(vsa_engine* engine, vsa_source_debug* out, uint32_t capacity,
                                                   uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_count must not be null");
        }
        *out_count = 0;
        check_out_array(out, capacity, "vsa_source_debug");
        vsa::world::DirectSimulator* direct = engine_of(engine).direct();
        const std::vector<vsa::world::SourceDebug> sources = direct != nullptr ? direct->sources() : std::vector<vsa::world::SourceDebug>{};
        *out_count = static_cast<uint32_t>(sources.size());
        for (std::size_t i = 0; i < capacity && i < sources.size(); ++i) {
            const vsa::world::SourceDebug& s = sources[i];
            vsa_source_debug d{};
            d.struct_size = sizeof d;
            d.flags = s.escaped ? VSA_SOURCE_ESCAPED : 0u;
            d.voice = s.voice;
            std::copy_n(s.position, 3, d.position);
            std::copy_n(s.simulated_position, 3, d.simulated_position);
            d.occlusion = s.occlusion;
            std::copy_n(s.transmission, 3, d.transmission);
            d.solid_metres = s.solid_metres;
            d.crossings = s.crossings;
            d.air_path = s.air_path;
            out[i] = d;
        }
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_get_simulation_stats(vsa_engine* engine, vsa_simulation_stats* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_simulation_stats");
        vsa::world::DirectSimulator* direct = engine_of(engine).direct();
        const vsa::world::SimulationStats s = direct != nullptr ? direct->stats() : vsa::world::SimulationStats{};
        vsa_simulation_stats stats{};
        stats.struct_size = sizeof stats;
        stats.sources = s.sources;
        stats.ticks = s.ticks;
        stats.last_tick_ms = s.last_tick_ms;
        stats.max_tick_ms = s.max_tick_ms;
        stats.occlusion_ms = s.occlusion_ms;
        stats.transmission_ms = s.transmission_ms;
        stats.rate_hz = s.rate_hz;
        stats.occlusion_samples = s.occlusion_samples;
        std::copy_n(s.listener, 3, stats.listener);
        std::copy_n(s.origin, 3, stats.origin);
        *out = stats;
        return VSA_OK;
    });
}

// ---- Reflections ----

VSA_API vsa_result VSA_CALL vsa_engine_get_reflection_stats(vsa_engine* engine, vsa_reflection_stats* out) {
    return guarded([&] {
        check_out_struct(out, "vsa_reflection_stats");
        const vsa::Engine::ReflectionReport r = engine_of(engine).reflection_report();
        vsa_reflection_stats stats{};
        stats.struct_size = sizeof stats;
        stats.enabled = r.enabled ? 1u : 0u;
        stats.slots = r.stats.slots;
        stats.live_slots = r.live;
        stats.waiting_slots = r.waiting;
        stats.draining_slots = r.draining;
        const vsa::world::ReflectionSettings& settings = r.stats.settings;
        stats.rays = settings.rays;
        stats.bounces = settings.bounces;
        stats.order = settings.order;
        stats.rate_hz = settings.rate_hz;
        stats.threads = settings.threads;
        stats.duration = settings.duration;
        stats.transition = settings.transition;
        stats.ticks = r.stats.ticks;
        stats.last_tick_ms = r.stats.last_tick_ms;
        stats.max_tick_ms = r.stats.max_tick_ms;
        stats.simulate_ms = r.stats.simulate_ms;
        std::copy_n(r.stats.listener_reverb_times, 3, stats.listener_reverb_times);
        stats.output_db = r.mean_square > 1e-12f ? 10.0f * std::log10(r.mean_square) : -120.0f;
        stats.gain = r.gain;
        std::copy_n(r.stats.listener, 3, stats.listener);
        *out = stats;
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_get_reflection_sources(vsa_engine* engine, vsa_reflection_source* out,
                                                              uint32_t capacity, uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "out_count must not be null");
        }
        *out_count = 0;
        check_out_array(out, capacity, "vsa_reflection_source");
        const std::vector<vsa::world::ReflectionSlotDebug> slots = engine_of(engine).reflection_slots();
        *out_count = static_cast<uint32_t>(slots.size());
        for (std::size_t i = 0; i < capacity && i < slots.size(); ++i) {
            const vsa::world::ReflectionSlotDebug& s = slots[i];
            vsa_reflection_source d{};
            d.struct_size = sizeof d;
            d.slot = s.slot;
            d.voice = s.voice;
            std::copy_n(s.position, 3, d.position);
            std::copy_n(s.reverb_times, 3, d.reverb_times);
            std::copy_n(s.eq, 3, d.eq);
            d.delay = s.delay;
            out[i] = d;
        }
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_set_reflection_gain(vsa_engine* engine, float gain) {
    return guarded([&] {
        engine_of(engine).set_reflection_gain(gain);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_engine_set_reflection_mix(vsa_engine* engine, float early_gain, float tail_gain) {
    return guarded([&] {
        engine_of(engine).set_reflection_mix(early_gain, tail_gain);
        return VSA_OK;
    });
}

VSA_API vsa_result VSA_CALL vsa_scene_trace_rays(vsa_engine* engine, const float origin[3], uint32_t rays,
                                                 uint32_t bounces, float max_distance, vsa_ray_segment* out,
                                                 uint32_t capacity, uint32_t* out_count) {
    return guarded([&] {
        if (out_count == nullptr || origin == nullptr) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "origin and out_count must not be null");
        }
        *out_count = 0;
        check_out_array(out, capacity, "vsa_ray_segment");
        if (rays > 4096 || bounces > 64 || !(max_distance > 0.0f) || !std::isfinite(max_distance)) {
            throw vsa::Error(VSA_ERROR_INVALID_ARGUMENT, "rays must be at most 4096, bounces at most 64, and max_distance positive");
        }
        const auto view = engine_of(engine).scene().voxel_view();
        const std::vector<vsa::world::RaySegment> segments =
            vsa::world::trace_paths(*view, origin, rays, bounces, max_distance, capacity);
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const vsa::world::RaySegment& s = segments[i];
            vsa_ray_segment d{};
            d.struct_size = sizeof d;
            d.bounce = s.bounce;
            std::copy_n(s.from, 3, d.from);
            std::copy_n(s.to, 3, d.to);
            d.energy = s.energy;
            d.material = s.material;
            out[i] = d;
        }
        *out_count = static_cast<uint32_t>(segments.size());
        return VSA_OK;
    });
}

VSA_API const char* VSA_CALL vsa_get_last_error(void) { return vsa::last_error(); }

}  // extern "C"
