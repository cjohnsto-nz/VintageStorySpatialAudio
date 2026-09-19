// The render thread's contract: no allocations, and a 256-voice load renders faster than real time.

#include "audio/asset.hpp"
#include "core/alloc_counter.hpp"
#include "engine.hpp"
#include "support/signals.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace {

vsa_engine_config make_config(uint32_t max_voices = 0) {
    vsa_engine_config config{};
    config.struct_size = sizeof config;
    config.abi_version = VSA_ABI_VERSION;
    config.ray_tracer = VSA_RAY_TRACER_STEAM;
    config.max_voices = max_voices;
    return config;
}

/// Asset handle that releases its creator reference.
struct AssetRef {
    vsa::Asset* asset = nullptr;
    explicit AssetRef(vsa::Asset* a) : asset(a) {}
    ~AssetRef() {
        if (asset != nullptr) {
            asset->release();
        }
    }
    AssetRef(const AssetRef&) = delete;
    AssetRef& operator=(const AssetRef&) = delete;
};

vsa::Asset* pcm_asset(vsa::Engine& engine, const std::vector<float>& samples, uint32_t channels, uint32_t rate) {
    const std::vector<int16_t> pcm = vsa_test::to_s16(samples);
    vsa_asset_desc desc{};
    desc.struct_size = sizeof desc;
    desc.format = VSA_ASSET_FORMAT_PCM_S16;
    desc.data = pcm.data();
    desc.size = pcm.size() * sizeof(int16_t);
    desc.pcm_channels = channels;
    desc.pcm_sample_rate = rate;
    return engine.create_asset(desc);
}

vsa::Asset* ogg_asset(vsa::Engine& engine, const std::vector<uint8_t>& ogg, uint32_t storage) {
    vsa_asset_desc desc{};
    desc.struct_size = sizeof desc;
    desc.format = VSA_ASSET_FORMAT_OGG_VORBIS;
    desc.storage = storage;
    desc.data = ogg.data();
    desc.size = ogg.size();
    return engine.create_asset(desc);
}

vsa_voice voice(vsa::Engine& engine, vsa::Asset* asset, uint32_t bus, float gain, float pitch, bool looping) {
    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = reinterpret_cast<vsa_asset*>(asset);
    desc.bus = bus;
    desc.gain = gain;
    desc.pitch = pitch;
    desc.looping = looping ? 1u : 0u;
    return engine.create_voice(desc);
}

}  // namespace

TEST_CASE("the render path never allocates") {
    vsa::Engine engine(make_config());
    AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 44100.0, 22050), 1, 44100));
    AssetRef stereo(pcm_asset(engine, vsa_test::sine(660.0, 48000.0, 9600, 0.4f, 2), 2, 48000));
    const auto ogg = vsa_test::ogg_file(vsa_test::sine(330.0, 44100.0, 88200, 0.4f, 2), 2, 44100);
    AssetRef streamed(ogg_asset(engine, ogg, VSA_ASSET_STORAGE_STREAMED));

    std::vector<vsa_voice> voices;
    voices.push_back(voice(engine, mono.asset, VSA_BUS_SOUND, 0.5f, 1.0f, true));
    voices.push_back(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.5f, 2.5f, false));
    voices.push_back(voice(engine, stereo.asset, VSA_BUS_AMBIENT, 0.3f, 0.7f, true));
    voices.push_back(voice(engine, streamed.asset, VSA_BUS_MUSIC, 0.8f, 1.0f, true));
    for (const vsa_voice v : voices) {
        engine.start_voice(v);
    }

    std::vector<float> out(48000 * 2);
    std::vector<uint64_t> allocations;
    // Exercise every command type between renders; only the renders are counted.
    for (int round = 0; round < 6; ++round) {
        switch (round) {
            case 1:
                engine.fade_voice(voices[0], 0.05f, 0.3f, 0, 7);
                engine.set_voice_pitch(voices[2], 1.3f);
                break;
            case 2:
                engine.pause_voice(voices[2]);
                engine.seek_voice(voices[3], 1.0);
                engine.set_bus_gain(VSA_BUS_MUSIC, 0.5f);
                break;
            case 3:
                engine.start_voice(voices[2]);
                engine.stop_voice(voices[1]);
                engine.set_master_gain(8.0f);  // into the limiter
                break;
            case 4:
                engine.set_voice_looping(voices[0], false);
                engine.release_voice(voices[2]);
                break;
            case 5: engine.set_voice_gain(voices[3], 0.2f); break;
            default: break;
        }
        vsa_test::AllocationScope scope;
        engine.render_offline(out.data(), 12000);
        allocations.push_back(scope.count());
    }
    for (const uint64_t count : allocations) {
        CHECK(count == 0);
    }
    const vsa_engine_stats stats = engine.stats();
    CHECK(stats.stream_underruns == 0);
    CHECK(stats.limiter_peak_reduction_db < -1.0f);  // the +18 dB master gain drove the limiter
}

TEST_CASE("256 voices render faster than real time") {
    vsa::Engine engine(make_config());
    constexpr int kVoices = 256;
    AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 44100.0, 44100, 0.1f), 1, 44100));
    AssetRef stereo(pcm_asset(engine, vsa_test::sine(523.0, 44100.0, 44100, 0.1f, 2), 2, 44100));
    for (int i = 0; i < kVoices; ++i) {
        // Mostly the common case (44.1 kHz assets on a 48 kHz engine, near unit pitch); every
        // eighth voice pitched up, which stretches the kernel and costs the most.
        const float pitch = i % 8 == 0 ? 1.8f : 0.9f + 0.001f * static_cast<float>(i);
        const vsa_voice v = voice(engine, i % 4 == 0 ? stereo.asset : mono.asset, static_cast<uint32_t>(i) % VSA_BUS_COUNT,
                                  0.02f, pitch, true);
        engine.start_voice(v);
    }

    const uint32_t block = engine.settings().block_frames;
    std::vector<float> out(block * 2);
    engine.render_offline(out.data(), block);  // apply the start commands

    std::vector<double> times_us;
    const int blocks = 2 * 48000 / static_cast<int>(block);
    for (int i = 0; i < blocks; ++i) {
        const auto start = std::chrono::steady_clock::now();
        engine.render_offline(out.data(), block);
        times_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times_us.begin(), times_us.end());
    const double period_us = 1e6 * block / 48000.0;
    const double p50 = times_us[times_us.size() / 2];
    const double p99 = times_us[times_us.size() * 99 / 100];
    const double active = engine.stats().active_voices;
    MESSAGE("256 voices, " << block << "-frame blocks (" << period_us << " us): p50 " << p50 << " us ("
                           << 100.0 * p50 / period_us << " %), p99 " << p99 << " us (" << 100.0 * p99 / period_us
                           << " %)");
    CHECK(active == kVoices);
#if defined(NDEBUG)
    // Optimised builds only; Debug (and the Debug-only sanitizer build) is far slower by design.
    CHECK(p99 < period_us);
#endif
}
