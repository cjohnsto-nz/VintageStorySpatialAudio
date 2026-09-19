// Output selection: the offline output's formats, device enumeration and (when the machine has
// one) a real device. Device tests degrade to a message on machines without audio hardware.

#include "engine_fixture.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace vsa_test;

TEST_CASE("the engine starts on the offline output at the configured rate") {
    OfflineEngine e;
    const vsa_engine_stats stats = e.stats();
    CHECK(stats.output_kind == VSA_OUTPUT_NONE);
    CHECK(stats.sample_rate == 48000);
    CHECK(stats.channels == 2);
    CHECK(stats.block_frames == 256);
    CHECK(stats.max_voices == 4096);
    CHECK(stats.device_period_frames == 0);
    CHECK(std::strlen(stats.device_name) == 0);
    CHECK(stats.block_period_us == doctest::Approx(5333.33).epsilon(1e-4));

    e.render(1000);
    CHECK(e.stats().blocks_rendered == 4);
}

TEST_CASE("the offline output can change rate and channel layout") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(sine(480.0, 48000.0, 4800, 0.5f), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);

    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = VSA_OUTPUT_NONE;
    desc.channels = 6;
    desc.sample_rate = 44100;
    REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
    CHECK(e.stats().sample_rate == 44100);
    CHECK(e.stats().channels == 6);

    std::vector<float> out(9600 * 6);
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), 9600) == VSA_OK);
    const auto front_left = channel(out, 6, 0);
    const auto centre = channel(out, 6, 2);
    // 480 Hz at 44.1 kHz: the voice was resampled to the new rate.
    CHECK(fit_sine(front_left.data() + 2000, 7000, 480.0, 44100.0).amplitude ==
          doctest::Approx(0.5 * kMonoPan).epsilon(0.002));
    CHECK(peak(centre) == 0.0);

    desc.channels = 3;
    CHECK(vsa_output_open(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    desc.channels = 2;
    desc.sample_rate = 96000;  // Steam Audio's HRTF supports 44.1 and 48 kHz only
    CHECK(vsa_output_open(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    desc.sample_rate = 0;
    desc.channels = 2;
    desc.kind = 9;
    CHECK(vsa_output_open(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("device enumeration returns well-formed records") {
    OfflineEngine e;
    uint32_t count = 0;
    const vsa_result first = vsa_device_enumerate(e.engine, nullptr, 0, &count);
    if (first == VSA_ERROR_DEVICE) {
        MESSAGE("no audio backend on this machine: " << vsa_get_last_error());
        return;
    }
    REQUIRE(first == VSA_OK);
    MESSAGE(count << " playback device(s)");

    std::vector<vsa_device_info> devices(count + 1);
    devices[0].struct_size = sizeof(vsa_device_info);
    uint32_t written = 0;
    REQUIRE(vsa_device_enumerate(e.engine, devices.data(), static_cast<uint32_t>(devices.size()), &written) == VSA_OK);
    CHECK(written == count);
    int defaults = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(devices[i].struct_size == sizeof(vsa_device_info));
        CHECK(std::strlen(devices[i].name) > 0);
        defaults += devices[i].is_default != 0 ? 1 : 0;
    }
    CHECK(defaults <= 1);
}

TEST_CASE("a real device renders on its own thread; offline rendering is refused meanwhile") {
    OfflineEngine e;
    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = VSA_OUTPUT_DEVICE;
    if (vsa_output_open(e.engine, &desc) != VSA_OK) {
        MESSAGE("no playback device available: " << vsa_get_last_error());
        CHECK(e.stats().output_kind == VSA_OUTPUT_NONE);  // a failed open leaves the engine usable
        return;
    }
    vsa_engine_stats stats = e.stats();
    CHECK(stats.output_kind == VSA_OUTPUT_DEVICE);
    CHECK(stats.sample_rate >= 8000);
    CHECK((stats.channels == 2 || stats.channels == 4 || stats.channels == 6 || stats.channels == 8 ||
           stats.channels == 12));
    MESSAGE("device '" << stats.device_name << "': " << stats.sample_rate << " Hz, " << stats.channels
                       << " channels, period " << stats.device_period_frames);

    float out[512] = {};
    CHECK(vsa_engine_render_offline(e.engine, out, 256) == VSA_ERROR_INVALID_STATE);

    const uint64_t before = stats.blocks_rendered;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stats = e.stats();
    CHECK(stats.blocks_rendered > before);

    desc.kind = VSA_OUTPUT_NONE;
    REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
    CHECK(vsa_engine_render_offline(e.engine, out, 256) == VSA_OK);
}

TEST_CASE("the spatial output runs a 7.1.4 bed where Windows Spatial Audio is enabled, else the device") {
    CapturedLog log;
    OfflineEngine e(make_config(VSA_RAY_TRACER_STEAM, &log));
    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = VSA_OUTPUT_SPATIAL;
    if (vsa_output_open(e.engine, &desc) != VSA_OK) {
        MESSAGE("no playback device available: " << vsa_get_last_error());
        CHECK(e.stats().output_kind == VSA_OUTPUT_NONE);
        return;
    }
    vsa_engine_stats stats = e.stats();
    const std::string path = stats.output_kind == VSA_OUTPUT_SPATIAL ? "spatial" : "fallback device";
    MESSAGE(path << " '" << stats.device_name << "': " << stats.sample_rate << " Hz, " << stats.channels
                 << " channels, period " << stats.device_period_frames);
    CHECK((stats.output_kind == VSA_OUTPUT_SPATIAL || stats.output_kind == VSA_OUTPUT_DEVICE));
    if (stats.output_kind == VSA_OUTPUT_SPATIAL) {
        CHECK(stats.channels == 12);
        CHECK((stats.sample_rate == 44100 || stats.sample_rate == 48000));
        CHECK(log.contains("spatial output:"));
    } else {
        CHECK(log.contains("spatial audio unavailable"));
    }
    const uint64_t before = stats.blocks_rendered;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stats = e.stats();
    CHECK(stats.blocks_rendered > before);
    CHECK(stats.output_kind != VSA_OUTPUT_NONE);

    desc.kind = VSA_OUTPUT_NONE;
    REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
    float out[512] = {};
    CHECK(vsa_engine_render_offline(e.engine, out, 256) == VSA_OK);
}

TEST_CASE("unknown output kinds are rejected") {
    OfflineEngine e;
    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = 3;
    CHECK(vsa_output_open(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    desc.kind = VSA_OUTPUT_NONE;
    desc.channels = 10;
    CHECK(vsa_output_open(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
}
