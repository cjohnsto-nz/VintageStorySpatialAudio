#include "engine_fixture.hpp"

#include <cstring>
#include <string>

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
