#pragma once

#include "support/signals.hpp"
#include "vsaudio.h"

#include <doctest/doctest.h>

#include <memory>
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

struct AssetDeleter {
    void operator()(vsa_asset* asset) const noexcept { vsa_asset_release(asset); }
};
using AssetPtr = std::unique_ptr<vsa_asset, AssetDeleter>;

/// Output latency of the master limiter at 48 kHz (detector lag 4 + 2 ms look-ahead - 1).
inline constexpr std::size_t kLimiterLatency = 99;
inline constexpr double kMonoPan = 0.70710678;

/// An engine on the offline (NONE) output at 48 kHz stereo, with helpers.
struct OfflineEngine : ScopedEngine {
    explicit OfflineEngine(vsa_engine_config config = make_config(VSA_RAY_TRACER_STEAM)) : ScopedEngine(config) {
        REQUIRE(result == VSA_OK);
    }

    /// Interleaved stereo.
    std::vector<float> render(std::size_t frames) {
        std::vector<float> out(frames * 2);
        REQUIRE(vsa_engine_render_offline(engine, out.data(), static_cast<uint32_t>(frames)) == VSA_OK);
        return out;
    }

    AssetPtr asset(const std::vector<uint8_t>& bytes, uint32_t storage = VSA_ASSET_STORAGE_AUTO,
                   uint32_t format = VSA_ASSET_FORMAT_AUTO) {
        vsa_asset_desc desc{};
        desc.struct_size = sizeof desc;
        desc.format = format;
        desc.storage = storage;
        desc.data = bytes.data();
        desc.size = bytes.size();
        desc.name = "test";
        vsa_asset* out = nullptr;
        const vsa_result r = vsa_asset_create(engine, &desc, &out);
        INFO(vsa_get_last_error());
        REQUIRE(r == VSA_OK);
        return AssetPtr(out);
    }

    AssetPtr pcm(const std::vector<float>& samples, uint32_t channels, uint32_t rate) {
        const std::vector<int16_t> s16 = to_s16(samples);
        vsa_asset_desc desc{};
        desc.struct_size = sizeof desc;
        desc.format = VSA_ASSET_FORMAT_PCM_S16;
        desc.data = s16.data();
        desc.size = s16.size() * sizeof(int16_t);
        desc.pcm_channels = channels;
        desc.pcm_sample_rate = rate;
        vsa_asset* out = nullptr;
        REQUIRE(vsa_asset_create(engine, &desc, &out) == VSA_OK);
        return AssetPtr(out);
    }

    vsa_voice voice(const AssetPtr& asset, float gain = 1.0f, bool looping = false, float pitch = 1.0f,
                    uint32_t bus = VSA_BUS_SOUND) {
        vsa_voice_desc desc{};
        desc.struct_size = sizeof desc;
        desc.asset = asset.get();
        desc.bus = bus;
        desc.gain = gain;
        desc.pitch = pitch;
        desc.looping = looping ? 1u : 0u;
        vsa_voice v = 0;
        const vsa_result r = vsa_voice_create(engine, &desc, &v);
        INFO(vsa_get_last_error());
        REQUIRE(r == VSA_OK);
        REQUIRE(v != 0);
        return v;
    }

    /// A looping positional voice.
    vsa_voice positioned(const AssetPtr& asset, uint32_t spatial, float x, float y, float z, float min_distance = 1.0f,
                         float gain = 1.0f) {
        vsa_voice_desc desc{};
        desc.struct_size = sizeof desc;
        desc.asset = asset.get();
        desc.bus = VSA_BUS_SOUND;
        desc.gain = gain;
        desc.pitch = 1.0f;
        desc.looping = 1;
        desc.spatial = spatial;
        desc.position[0] = x;
        desc.position[1] = y;
        desc.position[2] = z;
        desc.min_distance = min_distance;
        vsa_voice v = 0;
        const vsa_result r = vsa_voice_create(engine, &desc, &v);
        INFO(vsa_get_last_error());
        REQUIRE(r == VSA_OK);
        return v;
    }

    void listener(float px, float py, float pz, float fx, float fy, float fz, float ux = 0.0f, float uy = 1.0f,
                  float uz = 0.0f, float ox = 0.0f, float oy = 0.0f, float oz = 0.0f) {
        vsa_listener l{};
        l.struct_size = sizeof l;
        l.position[0] = px;
        l.position[1] = py;
        l.position[2] = pz;
        l.forward[0] = fx;
        l.forward[1] = fy;
        l.forward[2] = fz;
        l.up[0] = ux;
        l.up[1] = uy;
        l.up[2] = uz;
        l.render_offset[0] = ox;
        l.render_offset[1] = oy;
        l.render_offset[2] = oz;
        REQUIRE(vsa_listener_set(engine, &l) == VSA_OK);
    }

    vsa_voice_status status(vsa_voice v) {
        vsa_voice_status s{};
        s.struct_size = sizeof s;
        REQUIRE(vsa_voice_get_status(engine, v, &s) == VSA_OK);
        return s;
    }

    std::vector<vsa_event> events() {
        std::vector<vsa_event> out(64);
        out[0].struct_size = sizeof(vsa_event);
        uint32_t count = 0;
        REQUIRE(vsa_engine_poll_events(engine, out.data(), static_cast<uint32_t>(out.size()), &count) == VSA_OK);
        out.resize(count);
        return out;
    }

    vsa_engine_stats stats() {
        vsa_engine_stats s{};
        s.struct_size = sizeof s;
        REQUIRE(vsa_engine_get_stats(engine, &s) == VSA_OK);
        return s;
    }
};

}  // namespace vsa_test
