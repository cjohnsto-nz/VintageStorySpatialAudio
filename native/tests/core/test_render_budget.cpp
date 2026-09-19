// The render thread's contract: no allocations, and a 256-voice load renders faster than real time.

#include "audio/asset.hpp"
#include "core/alloc_counter.hpp"
#include "engine.hpp"
#include "support/signals.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace {

vsa_engine_config make_config(uint32_t max_voices = 0) {
    vsa_engine_config config{};
    config.struct_size = sizeof config;
    config.abi_version = VSA_ABI_VERSION;
    config.ray_tracer = VSA_RAY_TRACER_STEAM;
    config.max_voices = max_voices;
    // Offline, the reflection and pathing simulations run on the rendering thread; their own
    // tests below.
    config.flags = VSA_ENGINE_FLAG_NO_REFLECTIONS | VSA_ENGINE_FLAG_NO_PATHING;
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

vsa_voice voice(vsa::Engine& engine, vsa::Asset* asset, uint32_t bus, float gain, float pitch, bool looping,
                uint32_t spatial = VSA_SPATIAL_NONE, float x = 0.0f, float z = 0.0f, float y = 3.0f) {
    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = reinterpret_cast<vsa_asset*>(asset);
    desc.bus = bus;
    desc.gain = gain;
    desc.pitch = pitch;
    desc.looping = looping ? 1u : 0u;
    desc.spatial = spatial;
    desc.position[0] = x;
    desc.position[1] = y;  // in the rooms below, standing on their floor
    desc.position[2] = z;
    return engine.create_voice(desc);
}

vsa_listener facing(float fx, float fz) {
    vsa_listener l{};
    l.struct_size = sizeof l;
    l.forward[0] = fx;
    l.forward[2] = fz;
    l.up[1] = 1.0f;
    return l;
}

struct LoadResult {
    double period_us;
    double p50;
    double p99;
};

/// Renders ~2 s block by block and reports per-block render times.
LoadResult measure(vsa::Engine& engine) {
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
    return {1e6 * block / 48000.0, times_us[times_us.size() / 2], times_us[times_us.size() * 99 / 100]};
}

}  // namespace

TEST_CASE("the render path never allocates") {
    // One binaural place: of the two positional voices, one takes the world ambisonic bus. The
    // direct simulation is off: offline, it runs on this thread, and adding and removing sources
    // allocates (the steady state is checked below).
    vsa_engine_config config = make_config();
    config.max_binaural_voices = 1;
    config.flags |= VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION;
    vsa::Engine engine(config);
    AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 44100.0, 22050), 1, 44100));
    AssetRef stereo(pcm_asset(engine, vsa_test::sine(660.0, 48000.0, 9600, 0.4f, 2), 2, 48000));
    const auto ogg = vsa_test::ogg_file(vsa_test::sine(330.0, 44100.0, 88200, 0.4f, 2), 2, 44100);
    AssetRef streamed(ogg_asset(engine, ogg, VSA_ASSET_STORAGE_STREAMED));
    // Beds (5.1): through the head bus on headphones, from the speakers once they are on.
    AssetRef bed(pcm_asset(engine, vsa_test::sine(220.0, 48000.0, 9600, 0.3f, 6), 6, 48000));
    const auto bed_ogg = vsa_test::ogg_file(vsa_test::sine(250.0, 44100.0, 88200, 0.3f, 6), 6, 44100);
    AssetRef streamed_bed(ogg_asset(engine, bed_ogg, VSA_ASSET_STORAGE_STREAMED));

    std::vector<vsa_voice> voices;
    voices.push_back(voice(engine, mono.asset, VSA_BUS_SOUND, 0.5f, 1.0f, true));
    voices.push_back(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.5f, 2.5f, false));
    voices.push_back(voice(engine, stereo.asset, VSA_BUS_AMBIENT, 0.3f, 0.7f, true));
    voices.push_back(voice(engine, streamed.asset, VSA_BUS_MUSIC, 0.8f, 1.0f, true));
    voices.push_back(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.7f, 1.1f, true, VSA_SPATIAL_WORLD, 3.0f, -2.0f));
    voices.push_back(voice(engine, stereo.asset, VSA_BUS_WEATHER, 0.7f, 0.9f, true, VSA_SPATIAL_LISTENER, -1.0f, 0.0f));
    voices.push_back(voice(engine, bed.asset, VSA_BUS_WEATHER, 0.6f, 1.0f, true));
    voices.push_back(voice(engine, streamed_bed.asset, VSA_BUS_AMBIENT, 0.5f, 1.0f, true));
    for (const vsa_voice v : voices) {
        engine.start_voice(v);
    }

    std::vector<float> out(48000 * 2);
    std::vector<uint64_t> allocations;
    // Exercise every command type between renders; only the renders are counted.
    for (int round = 0; round < 7; ++round) {
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
            case 5:
                engine.set_voice_gain(voices[3], 0.2f);
                engine.set_listener(facing(1.0f, 0.0f));
                engine.set_voice_position(voices[4], VSA_SPATIAL_WORLD, -5.0f, 1.0f, 2.0f);
                engine.set_voice_lowpass(voices[5], 0.06f);
                engine.set_voice_lowpass(voices[6], 0.2f);
                engine.set_render_mode(VSA_RENDER_SPEAKERS);
                break;
            case 6: {
                vsa_output_desc desc{};  // 7.1.4: VBAP panning (the reopen itself may allocate)
                desc.struct_size = sizeof desc;
                desc.kind = VSA_OUTPUT_NONE;
                desc.channels = 12;
                engine.open_output(desc);
                out.resize(48000 * 12);
                break;
            }
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

TEST_CASE("the direct simulation's steady state never allocates") {
    // Positional voices playing and moving, with the simulation ticking on the rendering thread
    // (offline): once the sources exist, neither the simulation nor the render path allocates.
    vsa::Engine engine(make_config());
    AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 48000.0, 48000, 0.2f), 1, 48000));
    std::vector<vsa_voice> voices;
    for (int i = 0; i < 8; ++i) {
        voices.push_back(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.5f, 1.0f, true, VSA_SPATIAL_WORLD,
                               static_cast<float>(i) - 4.0f, -3.0f));
        engine.start_voice(voices.back());
    }
    std::vector<float> out(4800 * 2);
    for (int i = 0; i < 20; ++i) {
        engine.render_offline(out.data(), 4800);  // warm up: sources created and added
    }
    uint64_t allocations = 0;
    for (int round = 0; round < 10; ++round) {
        for (std::size_t i = 0; i < voices.size(); ++i) {
            engine.set_voice_position(voices[i], VSA_SPATIAL_WORLD, static_cast<float>(i) - 4.0f, 1.0f,
                                      -3.0f + 0.2f * static_cast<float>(round));
        }
        vsa_test::AllocationScope scope;
        engine.render_offline(out.data(), 4800);
        allocations += scope.count();
    }
    CHECK(allocations == 0);
    CHECK(engine.direct()->stats().sources == 8);
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

    const LoadResult r = measure(engine);
    MESSAGE("256 voices: p50 " << r.p50 << " us (" << 100.0 * r.p50 / r.period_us << " %), p99 " << r.p99 << " us ("
                               << 100.0 * r.p99 / r.period_us << " %) of " << r.period_us << " us");
    CHECK(engine.stats().active_voices == kVoices);
#if defined(NDEBUG)
    // Optimised builds only; Debug (and the Debug-only sanitizer build) is far slower by design.
    CHECK(r.p99 < r.period_us);
#endif
}

TEST_CASE("256 positional voices render faster than real time, binaural and panned") {
    for (const vsa_render_mode mode : {VSA_RENDER_HEADPHONES, VSA_RENDER_SPEAKERS}) {
        vsa::Engine engine(make_config());
        engine.set_render_mode(mode);
        constexpr int kVoices = 256;
        AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 44100.0, 44100, 0.1f), 1, 44100));
        for (int i = 0; i < kVoices; ++i) {
            // Around the listener at 2..30 m, vanilla-like pitch spread.
            const double angle = 2.0 * 3.14159265 * i / kVoices;
            const auto radius = static_cast<float>(2.0 + 28.0 * (i % 16) / 15.0);
            const float pitch = 0.8f + 0.4f * static_cast<float>(i % 7) / 6.0f;
            const vsa_voice v = voice(engine, mono.asset, VSA_BUS_ENTITY, 0.05f, pitch, true, VSA_SPATIAL_WORLD,
                                      radius * static_cast<float>(std::cos(angle)), radius * static_cast<float>(std::sin(angle)));
            engine.start_voice(v);
        }
        const LoadResult r = measure(engine);
        MESSAGE(std::string(mode == VSA_RENDER_HEADPHONES ? "binaural (64-voice budget)" : "panned") << ": p50 " << r.p50 << " us ("
                << 100.0 * r.p50 / r.period_us << " %), p99 " << r.p99 << " us (" << 100.0 * r.p99 / r.period_us << " %)");
        CHECK(engine.stats().real_voices == kVoices);
#if defined(NDEBUG)
        CHECK(r.p99 < r.period_us);
#endif
    }
}

namespace {

/// A 12 x 6 x 12 stone room (interior from 2, 2, 2) on a ground at y 1 in the engine's scene,
/// closed or with a two-block doorway in its south wall (z 14) at x 7..8.
void build_room(vsa::Engine& engine, bool doorway = false) {
    using namespace vsa::world;
    AcousticMaterial air;
    air.kind = MaterialKind::Air;
    AcousticMaterial stone;
    stone.kind = MaterialKind::Solid;
    stone.absorption[0] = stone.absorption[1] = stone.absorption[2] = 0.15f;
    stone.scattering = 0.3f;
    stone.transmission[0] = stone.transmission[1] = stone.transmission[2] = 0.001f;
    engine.scene().set_materials({air, stone});
    auto c = std::make_shared<ChunkVoxels>();
    for (int z = 0; z < 32; ++z) {
        for (int x = 0; x < 32; ++x) {
            c->materials[static_cast<std::size_t>(cell_index(x, 1, z))] = 1;
        }
    }
    for (int y = 1; y <= 8; ++y) {
        for (int z = 1; z <= 14; ++z) {
            for (int x = 1; x <= 14; ++x) {
                const bool shell = x == 1 || x == 14 || y == 1 || y == 8 || z == 1 || z == 14;
                const bool door = doorway && z == 14 && (x == 7 || x == 8) && (y == 2 || y == 3);
                if (shell && !door) {
                    c->materials[static_cast<std::size_t>(cell_index(x, y, z))] = 1;
                }
            }
        }
    }
    engine.scene().set_chunk({0, 0, 0}, c, 0);
    REQUIRE(engine.scene().wait_idle(std::chrono::seconds(10)));
}

vsa_listener in_room() {
    vsa_listener l = facing(0.0f, -1.0f);
    l.position[0] = 8.0f;
    l.position[1] = 3.7f;
    l.position[2] = 8.0f;
    return l;
}

vsa_engine_config reflection_config() {
    vsa_engine_config config = make_config();
    config.flags = VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION | VSA_ENGINE_FLAG_NO_PATHING;  // reflections alone
    return config;
}

/// Renders until every per-voice reflection slot is live (the simulation runs on its own thread).
void settle(vsa::Engine& engine, uint32_t live) {
    std::vector<float> out(4800 * 12);
    for (int i = 0; i < 200 && engine.reflection_report().live < live; ++i) {
        engine.render_offline(out.data(), 4800);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(engine.reflection_report().live == live);
}

}  // namespace

TEST_CASE("the reflections' render path never allocates") {
    // The simulation runs on its own thread (as with a device): only rendering is counted.
    vsa_engine_config config = reflection_config();
    config.reflection_sources = 4;
    config.reflection_rays = 1024;
    config.reflection_bounces = 8;
    vsa::Engine engine(config);
    build_room(engine);
    engine.set_listener(in_room());
    engine.reflection_simulator()->set_threaded(true);
    AssetRef tone(pcm_asset(engine, vsa_test::sine(440.0, 48000.0, 48000, 0.2f), 1, 48000));
    AssetRef blip(pcm_asset(engine, vsa_test::sine(880.0, 48000.0, 4800, 0.2f), 1, 48000));
    // Six lasting sounds more than 3 m apart (six places wanted) with four place slots: the
    // louder take over the quieter's.
    std::vector<vsa_voice> voices;
    for (int i = 0; i < 6; ++i) {
        voices.push_back(voice(engine, tone.asset, VSA_BUS_ENTITY, 0.2f + 0.1f * static_cast<float>(i), 1.0f, true,
                               VSA_SPATIAL_WORLD, 3.0f + 4.0f * static_cast<float>(i % 3), i < 3 ? 5.0f : 10.0f));
        engine.start_voice(voices.back());
    }
    settle(engine, 4);

    std::vector<float> out(48000 * 12);
    std::vector<uint64_t> allocations;
    for (int round = 0; round < 8; ++round) {
        switch (round) {
            case 1:  // short sounds: the listener's reverb
                for (int i = 0; i < 4; ++i) {
                    engine.start_voice(voice(engine, blip.asset, VSA_BUS_ENTITY, 0.5f, 1.0f, false, VSA_SPATIAL_WORLD,
                                             6.0f, 6.0f + static_cast<float>(i)));
                }
                break;
            case 2:  // slots drain and pass to the next voices
                engine.stop_voice(voices[5]);
                engine.stop_voice(voices[4]);
                break;
            case 3:
                engine.set_reflection_gain(0.5f);
                engine.set_voice_position(voices[0], VSA_SPATIAL_WORLD, 10.0f, 3.0f, 10.0f);
                break;
            case 4:
                engine.set_render_mode(VSA_RENDER_SPEAKERS);  // the speaker decoder
                break;
            case 5: {
                vsa_output_desc desc{};  // 7.1.4 (the reopen itself may allocate)
                desc.struct_size = sizeof desc;
                desc.kind = VSA_OUTPUT_NONE;
                desc.channels = 12;
                engine.open_output(desc);
                engine.reflection_simulator()->set_threaded(true);  // closing the output stopped it
                break;
            }
            case 6:
                engine.start_voice(voices[5]);
                engine.set_render_mode(VSA_RENDER_HEADPHONES);
                break;
            default: break;
        }
        vsa_test::AllocationScope scope;
        engine.render_offline(out.data(), 4000);
        allocations.push_back(scope.count());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // simulation results arrive
    }
    for (const uint64_t count : allocations) {
        CHECK(count == 0);
    }
    CHECK(engine.reflection_report().stats.ticks > 5);
}

TEST_CASE("reflections render within budget at the Medium quality, every place in use") {
    // The phase's CPU budget (PLAN section 9): the render thread under 25 % of the block period at
    // p99 with 32 voices in a room, every one of Medium's 10 places in use; a simulation run
    // within its 100 ms period on the default threads (which, resting as long as it runs, keeps
    // the simulation under one core at two threads).
    for (const vsa_render_mode mode : {VSA_RENDER_HEADPHONES, VSA_RENDER_SPEAKERS}) {
        vsa::Engine engine(reflection_config());
        engine.set_render_mode(mode);
        build_room(engine);
        engine.set_listener(in_room());
        if (mode == VSA_RENDER_SPEAKERS) {
            vsa_output_desc desc{};
            desc.struct_size = sizeof desc;
            desc.kind = VSA_OUTPUT_NONE;
            desc.channels = 12;
            engine.open_output(desc);
        }
        engine.reflection_simulator()->set_threaded(true);
        AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 48000.0, 48000, 0.1f), 1, 48000));
        // Ten sounds on a grid 3.5 m apart (ten places), and the rest 1 m from one of them.
        for (int i = 0; i < 32; ++i) {
            const int at = i % 10;
            const float x = 2.75f + 3.5f * static_cast<float>(at % 4) + (i >= 10 ? 1.0f : 0.0f);
            const float z = 2.75f + 3.5f * static_cast<float>(at / 4);
            engine.start_voice(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.1f, 1.0f, true, VSA_SPATIAL_WORLD, x, z));
        }
        settle(engine, 10);
        const uint32_t block = engine.settings().block_frames;
        std::vector<float> out(static_cast<std::size_t>(block) * 12);
        // The best of five 2-second windows half a second apart: a transient load elsewhere (the
        // virus scanner reading freshly built binaries, a finishing build) must not decide the
        // render thread's own cost; a render that is slow throughout still fails.
        double p50 = std::numeric_limits<double>::infinity();
        double p99 = std::numeric_limits<double>::infinity();
        for (int window = 0; window < 5; ++window) {
            if (window > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::vector<double> times_us;
            for (int i = 0; i < 2 * 48000 / static_cast<int>(block); ++i) {
                const auto start = std::chrono::steady_clock::now();
                engine.render_offline(out.data(), block);
                times_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times_us.begin(), times_us.end());
            p50 = std::min(p50, times_us[times_us.size() / 2]);
            p99 = std::min(p99, times_us[times_us.size() * 99 / 100]);
        }
        const double period = 1e6 * block / 48000.0;
        const vsa::Engine::ReflectionReport report = engine.reflection_report();
        MESSAGE(std::string(mode == VSA_RENDER_HEADPHONES ? "headphones" : "7.1.4") << ", 32 voices at 10 places: render p50 "
                << p50 << " us (" << 100.0 * p50 / period << " %), p99 " << p99 << " us (" << 100.0 * p99 / period
                << " %); simulation " << report.stats.last_tick_ms << " ms (worst " << report.stats.max_tick_ms << " ms)");
        CHECK(report.live == 10);
#if defined(NDEBUG)
        CHECK(p99 < 0.25 * period);
        CHECK(report.stats.last_tick_ms < 100.0);
#endif
    }
}

namespace {

vsa_engine_config pathing_config() {
    vsa_engine_config config = make_config();
    config.flags = VSA_ENGINE_FLAG_NO_REFLECTIONS;  // pathing, and the direct simulation that asks for it
    config.pathing_range = 32;
    config.pathing_height = 16;
    return config;
}

/// Outside the room's south wall, 4 m from the doorway, looking at it.
vsa_listener at_doorway() {
    vsa_listener l = facing(0.0f, -1.0f);
    l.position[0] = 8.0f;
    l.position[1] = 3.7f;
    l.position[2] = 18.0f;
    return l;
}

/// Renders until `found` blocked sounds have a path (the baker and the simulation run on their
/// own threads).
void settle_paths(vsa::Engine& engine, uint32_t found) {
    std::vector<float> out(4800 * 12);
    for (int i = 0; i < 300 && engine.paths()->stats().found < found; ++i) {
        engine.render_offline(out.data(), 4800);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const vsa::world::PathSimStats s = engine.paths()->stats();
    const vsa::world::PathBakeStats b = engine.path_baker()->stats();
    MESSAGE("paths: ticks " << s.ticks << " wanted " << s.wanted << " simulated " << s.simulated << " found " << s.found << " batch " << s.batch_id
            << "; bakes " << b.bakes << " baking " << b.baking << " probes " << b.probes << " listener " << s.listener[0] << "," << s.listener[1] << "," << s.listener[2]);
    REQUIRE(s.found == found);
}

}  // namespace

TEST_CASE("the pathing render path never allocates") {
    // The bake and the simulation run on their own threads (as with a device): only rendering is
    // counted. Sounds in a room with a doorway, the listener outside: every one wants a path.
    vsa_engine_config config = pathing_config();
    config.pathing_sources = 4;
    vsa::Engine engine(config);
    build_room(engine, true);
    engine.set_listener(at_doorway());
    engine.path_baker()->set_threaded(true);
    engine.paths()->set_threaded(true);
    AssetRef tone(pcm_asset(engine, vsa_test::sine(440.0, 48000.0, 48000, 0.2f), 1, 48000));
    // Six blocked sounds with four path slots: the louder get theirs.
    std::vector<vsa_voice> voices;
    for (int i = 0; i < 6; ++i) {
        voices.push_back(voice(engine, tone.asset, VSA_BUS_ENTITY, 0.2f + 0.1f * static_cast<float>(i), 1.0f, true,
                               VSA_SPATIAL_WORLD, 3.0f + 4.0f * static_cast<float>(i % 3), i < 3 ? 5.0f : 10.0f));
        engine.start_voice(voices.back());
    }
    settle_paths(engine, 4);

    std::vector<float> out(48000 * 12);
    std::vector<uint64_t> allocations;
    for (int round = 0; round < 8; ++round) {
        switch (round) {
            case 1:  // a sound moves, another stops: paths pass on
                engine.set_voice_position(voices[5], VSA_SPATIAL_WORLD, 10.0f, 3.0f, 12.0f);
                engine.stop_voice(voices[4]);
                break;
            case 2:  // out through the doorway: no path wanted, the direct sound takes over
                engine.set_voice_position(voices[3], VSA_SPATIAL_WORLD, 8.0f, 3.0f, 16.0f);
                break;
            case 3:
                engine.set_render_mode(VSA_RENDER_SPEAKERS);  // the path decoder
                break;
            case 4: {
                vsa_output_desc desc{};  // 7.1.4 (the reopen itself may allocate)
                desc.struct_size = sizeof desc;
                desc.kind = VSA_OUTPUT_NONE;
                desc.channels = 12;
                engine.open_output(desc);
                engine.path_baker()->set_threaded(true);  // closing the output stopped them
                engine.paths()->set_threaded(true);
                break;
            }
            case 5:  // the listener walks on: a new box is baked and swapped in
                engine.set_listener([] {
                    vsa_listener l = at_doorway();
                    l.position[2] = 30.0f;
                    return l;
                }());
                break;
            case 6:
                engine.start_voice(voices[4]);
                engine.set_render_mode(VSA_RENDER_HEADPHONES);
                break;
            default: break;
        }
        vsa_test::AllocationScope scope;
        engine.render_offline(out.data(), 4000);
        allocations.push_back(scope.count());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // simulation results arrive
    }
    for (const uint64_t count : allocations) {
        CHECK(count == 0);
    }
    CHECK(engine.paths()->stats().ticks > 5);
    CHECK(engine.path_baker()->stats().bakes >= 2);
}

TEST_CASE("pathing renders within budget at the default quality, every path slot in use") {
    // PLAN section 9's budget for the phase: the render thread under 25 % of the block period at
    // p99 with 32 blocked voices, 16 of them (the default) rendered through path effects; a
    // simulation run within its 100 ms period.
    for (const vsa_render_mode mode : {VSA_RENDER_HEADPHONES, VSA_RENDER_SPEAKERS}) {
        vsa::Engine engine(pathing_config());
        engine.set_render_mode(mode);
        build_room(engine, true);
        engine.set_listener(at_doorway());
        if (mode == VSA_RENDER_SPEAKERS) {
            vsa_output_desc desc{};
            desc.struct_size = sizeof desc;
            desc.kind = VSA_OUTPUT_NONE;
            desc.channels = 12;
            engine.open_output(desc);
        }
        engine.path_baker()->set_threaded(true);
        engine.paths()->set_threaded(true);
        AssetRef mono(pcm_asset(engine, vsa_test::sine(440.0, 48000.0, 48000, 0.1f), 1, 48000));
        for (int i = 0; i < 32; ++i) {
            const float x = 3.0f + static_cast<float>(i % 8) * 1.4f;
            const float z = 3.0f + static_cast<float>(i / 8) * 2.5f;
            engine.start_voice(voice(engine, mono.asset, VSA_BUS_ENTITY, 0.1f, 1.0f, true, VSA_SPATIAL_WORLD, x, z));
        }
        settle_paths(engine, 16);
        const uint32_t block = engine.settings().block_frames;
        std::vector<float> out(static_cast<std::size_t>(block) * 12);
        double p50 = std::numeric_limits<double>::infinity();
        double p99 = std::numeric_limits<double>::infinity();
        for (int window = 0; window < 5; ++window) {
            if (window > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::vector<double> times_us;
            for (int i = 0; i < 2 * 48000 / static_cast<int>(block); ++i) {
                const auto start = std::chrono::steady_clock::now();
                engine.render_offline(out.data(), block);
                times_us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times_us.begin(), times_us.end());
            p50 = std::min(p50, times_us[times_us.size() / 2]);
            p99 = std::min(p99, times_us[times_us.size() * 99 / 100]);
        }
        const double period = 1e6 * block / 48000.0;
        const vsa::world::PathSimStats stats = engine.paths()->stats();
        MESSAGE(std::string(mode == VSA_RENDER_HEADPHONES ? "headphones" : "7.1.4") << ", 32 blocked voices, 16 paths: render p50 "
                << p50 << " us (" << 100.0 * p50 / period << " %), p99 " << p99 << " us (" << 100.0 * p99 / period
                << " %); simulation " << stats.last_tick_ms << " ms (worst " << stats.max_tick_ms << " ms), " << stats.wanted
                << " wanted");
        CHECK(stats.wanted > 16);  // the rest see the listener through the doorway
        CHECK(stats.found == 16);
#if defined(NDEBUG)
        CHECK(p99 < 0.25 * period);
        CHECK(stats.last_tick_ms < 100.0);
#endif
    }
}
