#include "engine_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace vsa_test;

TEST_CASE("vsa_get_version reports ABI and Steam Audio versions") {
    vsa_version_info info{};
    info.struct_size = sizeof info;
    REQUIRE(vsa_get_version(&info) == VSA_OK);
    CHECK(info.abi_version == VSA_ABI_VERSION);
    CHECK(info.steam_audio_major == 4);
    CHECK(info.steam_audio_minor == 8);
    CHECK(info.steam_audio_patch == 1);
    REQUIRE(info.build_description != nullptr);
    CHECK(std::strlen(info.build_description) > 0);
    CHECK(std::string(vsa_get_last_error()).empty());
}

TEST_CASE("struct_size mismatches are rejected, not trusted") {
    vsa_version_info info{};
    info.struct_size = sizeof info - 4;
    CHECK(vsa_get_version(&info) == VSA_ERROR_ABI_MISMATCH);
    CHECK(std::string(vsa_get_last_error()).find("struct_size") != std::string::npos);

    CHECK(vsa_get_version(nullptr) == VSA_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("engine config is validated") {
    vsa_engine* engine = reinterpret_cast<vsa_engine*>(0x1);

    SUBCASE("null config") {
        CHECK(vsa_engine_create(nullptr, &engine) == VSA_ERROR_INVALID_ARGUMENT);
        CHECK(engine == nullptr);
    }
    SUBCASE("null out pointer") {
        const auto config = make_config(VSA_RAY_TRACER_STEAM);
        CHECK(vsa_engine_create(&config, nullptr) == VSA_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("wrong ABI version") {
        auto config = make_config(VSA_RAY_TRACER_STEAM);
        config.abi_version = VSA_ABI_VERSION + 1;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_ABI_MISMATCH);
        CHECK(engine == nullptr);
    }
    SUBCASE("wrong struct size") {
        auto config = make_config(VSA_RAY_TRACER_STEAM);
        config.struct_size = 8;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_ABI_MISMATCH);
    }
    SUBCASE("unknown ray tracer") {
        auto config = make_config(VSA_RAY_TRACER_AUTO);
        config.ray_tracer = 42;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("render settings out of range") {
        auto config = make_config(VSA_RAY_TRACER_STEAM);
        config.block_frames = 16;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_INVALID_ARGUMENT);
        config.block_frames = 0;
        config.sample_rate = 96000;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_INVALID_ARGUMENT);
        config.sample_rate = 0;
        config.resampler_quality = 9;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_INVALID_ARGUMENT);
        config.resampler_quality = 0;
        config.max_voices = 1u << 20;
        CHECK(vsa_engine_create(&config, &engine) == VSA_ERROR_INVALID_ARGUMENT);
        CHECK(engine == nullptr);
    }
}

TEST_CASE("only one engine may exist at a time, and it can be recreated") {
    CapturedLog log;
    {
        ScopedEngine first(make_config(VSA_RAY_TRACER_STEAM, &log));
        REQUIRE(first.result == VSA_OK);
        REQUIRE(first.engine != nullptr);

        ScopedEngine second(make_config(VSA_RAY_TRACER_STEAM));
        CHECK(second.result == VSA_ERROR_ALREADY_EXISTS);
        CHECK(second.engine == nullptr);
    }
    ScopedEngine again(make_config(VSA_RAY_TRACER_STEAM));
    CHECK(again.result == VSA_OK);

    CHECK(log.contains("context created"));
    CHECK(log.contains("vsaudio"));
}

TEST_CASE("engine info reports a concrete ray tracer") {
    ScopedEngine scoped(make_config(VSA_RAY_TRACER_AUTO));
    REQUIRE(scoped.result == VSA_OK);

    vsa_engine_info info{};
    info.struct_size = sizeof info;
    REQUIRE(vsa_engine_get_info(scoped.engine, &info) == VSA_OK);
    CHECK(info.active_ray_tracer != static_cast<uint32_t>(VSA_RAY_TRACER_AUTO));
    if (info.embree_available != 0) {
        CHECK(info.active_ray_tracer == static_cast<uint32_t>(VSA_RAY_TRACER_EMBREE));
    } else {
        CHECK(info.active_ray_tracer == static_cast<uint32_t>(VSA_RAY_TRACER_STEAM));
    }
    MESSAGE("Embree available on this machine: " << std::string(info.embree_available != 0 ? "yes" : "no"));

    CHECK(vsa_engine_get_info(nullptr, &info) == VSA_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("the log sink is detached when the engine is destroyed") {
    CapturedLog log;
    {
        ScopedEngine scoped(make_config(VSA_RAY_TRACER_STEAM, &log));
        REQUIRE(scoped.result == VSA_OK);
    }
    const auto lines_after_destroy = [&] {
        std::lock_guard lock(log.mutex);
        return log.lines.size();
    }();
    // A new engine without a sink must not write into the old one.
    ScopedEngine next(make_config(VSA_RAY_TRACER_STEAM));
    REQUIRE(next.result == VSA_OK);
    std::lock_guard lock(log.mutex);
    CHECK(log.lines.size() == lines_after_destroy);
}

TEST_CASE("thread stats name the engine's threads and measure their CPU time") {
    OfflineEngine e(make_config(VSA_RAY_TRACER_AUTO));
    std::vector<vsa_thread_stats> threads(128);
    uint32_t count = 0;
    bool worker = false;
    bool builder = false;
    // The engine's threads register as they start, moments after the engine exists.
    for (int attempt = 0; attempt < 100 && !(worker && builder); ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        threads[0].struct_size = sizeof(vsa_thread_stats);
        REQUIRE(vsa_engine_get_thread_stats(e.engine, threads.data(), 128, &count) == VSA_OK);
        for (uint32_t i = 0; i < std::min(count, 128u); ++i) {
            const vsa_thread_stats& t = threads[i];
            CHECK(t.struct_size == sizeof(vsa_thread_stats));
            CHECK(t.name[0] != '\0');
            CHECK(t.cpu_ms >= 0.0);
            if (std::strcmp(t.name, "engine worker") == 0) {
                worker = t.kind == VSA_THREAD_ENGINE && t.thread_id != 0;
            }
            if (std::strcmp(t.name, "scene builder") == 0) {
                builder = t.kind == VSA_THREAD_ENGINE;
            }
        }
    }
    CHECK(worker);
    CHECK(builder);
    // The main thread is not the engine's (its start lies in this executable, not in vsaudio.dll).
    for (uint32_t i = 0; i < std::min(count, 128u); ++i) {
        CHECK(std::strcmp(threads[i].name, "engine (unnamed)") != 0);
    }
    // The count alone.
    uint32_t total = 0;
    REQUIRE(vsa_engine_get_thread_stats(e.engine, nullptr, 0, &total) == VSA_OK);
    CHECK(total == count);
}

TEST_CASE("the default config reports every setting resolved, and running it changes nothing") {
    vsa_engine_config defaults{};
    defaults.struct_size = sizeof defaults;
    REQUIRE(vsa_get_default_config(&defaults) == VSA_OK);
    CHECK(defaults.struct_size == sizeof defaults);
    // Nothing is left at zero but the fields where zero is the value: the flags, the ray tracer
    // (auto), the ABI version and the reserved word.
    CHECK(defaults.sample_rate == 48000);
    CHECK(defaults.block_frames == 256);
    CHECK(defaults.max_voices > 0);
    CHECK(defaults.max_real_voices > 0);
    CHECK(defaults.max_binaural_voices > 0);
    CHECK(defaults.stream_threshold_ms > 0);
    CHECK(defaults.occlusion_samples > 0);
    CHECK(defaults.direct_rate_hz > 0);
    CHECK(defaults.reflection_sources > 0);
    CHECK(defaults.reflection_rays >= 256);
    CHECK(defaults.reflection_bounces > 0);
    CHECK(defaults.reflection_duration > 0.0f);
    CHECK(defaults.reflection_order > 0);
    CHECK(defaults.reflection_rate_hz > 0);
    CHECK(defaults.reflection_threads > 0);  // from this machine's cores
    CHECK(defaults.reflection_transition > 0.0f);
    CHECK(defaults.pathing_range >= 32);
    CHECK(defaults.pathing_height >= 16);
    CHECK(defaults.pathing_probe_spacing > 0.0f);
    CHECK(defaults.pathing_vis_samples > 0);
    CHECK(defaults.pathing_rate_hz > 0);
    CHECK(defaults.pathing_sources > 0);
    CHECK(defaults.pathing_max_probes >= 64);

    // Every one of them is accepted, and resolves to itself: writing them into a settings file
    // and reading it back must not change what the engine does.
    defaults.abi_version = VSA_ABI_VERSION;
    defaults.ray_tracer = VSA_RAY_TRACER_AUTO;
    ScopedEngine e(defaults);
    vsa_engine_config again{};
    again.struct_size = sizeof again;
    REQUIRE(vsa_get_default_config(&again) == VSA_OK);
    CHECK(std::memcmp(&again, &defaults, sizeof again) == 0);
}

TEST_CASE("the sound inspector says how loud each voice is and which way it reaches the listener") {
    OfflineEngine e(make_config(VSA_RAY_TRACER_AUTO));
    const AssetPtr tone = e.pcm(sine(440.0, 48000.0, 48000, 0.5f), 1, 48000);
    const AssetPtr quiet = e.pcm(sine(660.0, 48000.0, 48000, 0.05f), 1, 48000);
    std::vector<vsa_audible_voice> rows(32);
    uint32_t count = 0;

    // Off by default: nothing reported, and the render thread does none of the work.
    REQUIRE(vsa_voice_start(e.engine, e.positioned(tone, VSA_SPATIAL_WORLD, 2.0f, 0.0f, 0.0f)) == VSA_OK);
    REQUIRE(vsa_voice_start(e.engine, e.positioned(quiet, VSA_SPATIAL_WORLD, 3.0f, 0.0f, 0.0f)) == VSA_OK);
    e.render(4800);
    rows[0].struct_size = sizeof(vsa_audible_voice);
    REQUIRE(vsa_engine_get_audible(e.engine, rows.data(), 32, &count) == VSA_OK);
    CHECK(count == 0);

    REQUIRE(vsa_engine_set_inspect(e.engine, 1) == VSA_OK);
    e.render(4800);
    rows[0].struct_size = sizeof(vsa_audible_voice);
    REQUIRE(vsa_engine_get_audible(e.engine, rows.data(), 32, &count) == VSA_OK);
    REQUIRE(count == 2);
    CHECK(rows[0].struct_size == sizeof(vsa_audible_voice));
    // Loudest first, and the louder tone really is the louder one.
    CHECK(rows[0].heard_db > rows[1].heard_db);
    CHECK(rows[0].heard_db > -60.0f);
    CHECK(rows[0].voice != rows[1].voice);
    // In the open, everything it is worth arrives by the direct way.
    CHECK(rows[0].direct_db == doctest::Approx(rows[0].heard_db).epsilon(0.05));
    CHECK(rows[0].distance == doctest::Approx(2.0f).epsilon(0.2));
    CHECK((rows[0].flags & VSA_AUDIBLE_HEAD_LOCKED) == 0);

    // A head-locked sound is reported as one, at no distance.
    REQUIRE(vsa_voice_start(e.engine, e.voice(tone, 1.0f, false, 1.0f, VSA_BUS_MUSIC)) == VSA_OK);
    e.render(4800);
    rows[0].struct_size = sizeof(vsa_audible_voice);
    REQUIRE(vsa_engine_get_audible(e.engine, rows.data(), 32, &count) == VSA_OK);
    REQUIRE(count == 3);
    const auto locked = std::find_if(rows.begin(), rows.begin() + count, [](const vsa_audible_voice& r) {
        return (r.flags & VSA_AUDIBLE_HEAD_LOCKED) != 0;
    });
    REQUIRE(locked != rows.begin() + count);
    CHECK(locked->distance == 0.0f);
    CHECK(locked->bus == VSA_BUS_MUSIC);

    // The count alone, and switching it off again.
    uint32_t total = 0;
    REQUIRE(vsa_engine_get_audible(e.engine, nullptr, 0, &total) == VSA_OK);
    CHECK(total == count);
    REQUIRE(vsa_engine_set_inspect(e.engine, 0) == VSA_OK);
    e.render(4800);
    rows[0].struct_size = sizeof(vsa_audible_voice);
    REQUIRE(vsa_engine_get_audible(e.engine, rows.data(), 32, &count) == VSA_OK);
    CHECK(count == 0);
}
