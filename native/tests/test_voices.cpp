// Voice semantics through the C ABI, rendered offline: state reporting, looping, fades,
// declicking, gains, the limiter, handles and capacity.

#include "engine_fixture.hpp"

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

using namespace vsa_test;

namespace {

// A 480 Hz sine at 48 kHz, whole periods only, so it loops seamlessly.
std::vector<float> loopable_sine(float amplitude = 0.5f) { return sine(480.0, 48000.0, 4800, amplitude); }

double window_rms_db(const std::vector<float>& left, double start_seconds, double length_seconds) {
    const auto first = static_cast<std::size_t>(start_seconds * 48000.0) + kLimiterLatency;
    const auto n = static_cast<std::size_t>(length_seconds * 48000.0);
    return to_db(rms(left.data() + first, n));
}

bool has_event(const std::vector<vsa_event>& events, vsa_event_type type, vsa_voice voice) {
    for (const vsa_event& e : events) {
        if (e.type == static_cast<uint32_t>(type) && e.voice == voice) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("voice status reflects commands immediately, before the render thread applies them") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);

    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(e.status(v).position_seconds == 0.0);

    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    CHECK(e.status(v).state == VSA_VOICE_PLAYING);  // nothing rendered yet

    e.render(4800);
    CHECK(e.status(v).state == VSA_VOICE_PLAYING);
    // Looping 0.1 s asset: 19 blocks of 256 frames = 0.1013 s wraps to ~0.0013 s.
    CHECK(e.status(v).position_seconds == doctest::Approx(4864.0 / 48000.0 - 0.1).epsilon(1e-6));

    REQUIRE(vsa_voice_pause(e.engine, v) == VSA_OK);
    CHECK(e.status(v).state == VSA_VOICE_PAUSED);
    e.render(2400);
    const double paused_at = e.status(v).position_seconds;
    e.render(2400);
    CHECK(e.status(v).position_seconds == paused_at);

    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    CHECK(e.status(v).state == VSA_VOICE_PLAYING);
    e.render(512);
    CHECK(e.status(v).position_seconds != paused_at);

    REQUIRE(vsa_voice_stop(e.engine, v) == VSA_OK);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(e.status(v).position_seconds == 0.0);
    e.render(1024);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(e.status(v).position_seconds == 0.0);

    // Pausing a stopped voice is a no-op, like OpenAL.
    REQUIRE(vsa_voice_pause(e.engine, v) == VSA_OK);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);

    REQUIRE(vsa_voice_release(e.engine, v) == VSA_OK);
    vsa_voice_status status{};
    status.struct_size = sizeof status;
    CHECK(vsa_voice_get_status(e.engine, v, &status) == VSA_ERROR_INVALID_HANDLE);
    CHECK(vsa_voice_start(e.engine, v) == VSA_ERROR_INVALID_HANDLE);
    CHECK(vsa_voice_release(e.engine, v) == VSA_ERROR_INVALID_HANDLE);
    CHECK(vsa_voice_start(e.engine, 0) == VSA_ERROR_INVALID_HANDLE);
}

TEST_CASE("a one-shot voice ends, rewinds and reports VOICE_ENDED; it can be played again") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(sine(1000.0, 48000.0, 4800), 1, 48000);  // 0.1 s
    const vsa_voice v = e.voice(asset);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto first = channel(e.render(14400), 2, 0);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(e.status(v).position_seconds == 0.0);
    CHECK(has_event(e.events(), VSA_EVENT_VOICE_ENDED, v));
    // Audible for 0.1 s, silent afterwards.
    CHECK(window_rms_db(first, 0.02, 0.06) > -13.0);  // 0.5 amplitude, -3 dB pan: -12.04 dB RMS
    CHECK(window_rms_db(first, 0.15, 0.1) < -120.0);

    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto second = channel(e.render(14400), 2, 0);
    CHECK(window_rms_db(second, 0.02, 0.06) == doctest::Approx(window_rms_db(first, 0.02, 0.06)).epsilon(0.001));
}

TEST_CASE("looping is seamless at the loop point, with and without resampling") {
    SUBCASE("48 kHz asset on a 48 kHz engine") {
        OfflineEngine e;
        const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
        const vsa_voice v = e.voice(asset, 1.0f, true);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const auto left = channel(e.render(48000), 2, 0);
        const std::vector<float> body(left.begin() + 1000, left.end());
        const SineFit fit = fit_sine(body.data(), body.size(), 480.0, 48000.0);
        CHECK(fit.amplitude == doctest::Approx(0.5 * kMonoPan).epsilon(0.001));
        CHECK(fit.thd_n_db() < -80.0);  // int16 source quantisation only
        CHECK(e.status(v).position_seconds < 0.1);
    }
    SUBCASE("44.1 kHz asset on a 48 kHz engine") {
        OfflineEngine e;
        const AssetPtr asset = e.pcm(sine(441.0, 44100.0, 4400), 1, 44100);  // 44 whole periods
        const vsa_voice v = e.voice(asset, 1.0f, true);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const auto left = channel(e.render(48000), 2, 0);
        const std::vector<float> body(left.begin() + 1000, left.end());
        const SineFit fit = fit_sine(body.data(), body.size(), 441.0, 48000.0);
        CHECK(fit.thd_n_db() < -60.0);
    }
}

TEST_CASE("pitch changes the playback rate") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true, 1.5f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto left = channel(e.render(24000), 2, 0);
    const std::vector<float> body(left.begin() + 1000, left.end());
    CHECK(fit_sine(body.data(), body.size(), 720.0, 48000.0).thd_n_db() < -60.0);

    REQUIRE(vsa_voice_set_pitch(e.engine, v, 0.5f) == VSA_OK);
    const auto slower = channel(e.render(24000), 2, 0);
    const std::vector<float> body2(slower.begin() + 1000, slower.end());
    CHECK(fit_sine(body2.data(), body2.size(), 240.0, 48000.0).thd_n_db() < -60.0);
}

TEST_CASE("a fade is linear in dB and posts FADE_DONE with its token") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    REQUIRE(vsa_voice_fade(e.engine, v, 0.01f, 1.0f, 0, 42) == VSA_OK);  // 0 dB -> -40 dB over 1 s
    const auto left = channel(e.render(60000), 2, 0);

    // RMS of the unfaded tone: 0.5 amplitude, -3 dB mono pan.
    const double full_db = to_db(0.5 * kMonoPan / std::numbers::sqrt2);
    for (const double t : {0.25, 0.5, 0.75}) {
        CAPTURE(t);
        CHECK(std::abs(window_rms_db(left, t - 0.01, 0.02) - (full_db - 40.0 * t)) < 0.3);
    }
    CHECK(std::abs(window_rms_db(left, 1.05, 0.1) - (full_db - 40.0)) < 0.3);

    const auto events = e.events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == VSA_EVENT_FADE_DONE);
    CHECK(events[0].voice == v);
    CHECK(events[0].token == 42);
    CHECK(events[0].flags == 0);
    CHECK(e.status(v).state == VSA_VOICE_PLAYING);
}

TEST_CASE("fade options: stop when done, and cancellation by set_gain") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);

    const vsa_voice a = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, a) == VSA_OK);
    REQUIRE(vsa_voice_fade(e.engine, a, 0.0f, 0.1f, VSA_FADE_STOP_WHEN_DONE, 1) == VSA_OK);
    e.render(9600);
    CHECK(e.status(a).state == VSA_VOICE_STOPPED);

    const vsa_voice b = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, b) == VSA_OK);
    REQUIRE(vsa_voice_fade(e.engine, b, 0.0f, 5.0f, 0, 2) == VSA_OK);
    e.render(2400);
    REQUIRE(vsa_voice_set_gain(e.engine, b, 0.5f) == VSA_OK);
    e.render(2400);

    bool stopped_done = false;
    bool cancelled = false;
    for (const vsa_event& event : e.events()) {
        stopped_done = stopped_done || (event.voice == a && event.token == 1 && event.flags == 0);
        cancelled = cancelled || (event.voice == b && event.token == 2 &&
                                  (event.flags & VSA_EVENT_FLAG_FADE_CANCELLED) != 0);
    }
    CHECK(stopped_done);
    CHECK(cancelled);
    CHECK(e.status(b).state == VSA_VOICE_PLAYING);
}

TEST_CASE("fades keep running while a voice is paused") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    REQUIRE(vsa_voice_fade(e.engine, v, 0.1f, 0.2f, 0, 5) == VSA_OK);
    REQUIRE(vsa_voice_pause(e.engine, v) == VSA_OK);
    e.render(14400);
    CHECK(has_event(e.events(), VSA_EVENT_FADE_DONE, v));
}

TEST_CASE("pause, resume, stop and seek are declicked") {
    OfflineEngine e;
    // Full-scale-ish low tone: a hard cut would jump by up to ~0.35.
    const AssetPtr asset = e.pcm(sine(100.0, 48000.0, 48000, 0.5f), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    const double natural_step = 2.0 * std::numbers::pi * 100.0 / 48000.0 * 0.5 * kMonoPan;

    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    std::vector<float> left = channel(e.render(1200), 2, 0);  // stop mid-block, near a peak
    const auto append = [&](std::size_t frames) {
        const auto more = channel(e.render(frames), 2, 0);
        left.insert(left.end(), more.begin(), more.end());
    };
    REQUIRE(vsa_voice_pause(e.engine, v) == VSA_OK);
    append(2400);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    append(2400);
    REQUIRE(vsa_voice_seek(e.engine, v, 0.3) == VSA_OK);
    CHECK(e.status(v).position_seconds == doctest::Approx(0.3));
    append(2400);
    REQUIRE(vsa_voice_stop(e.engine, v) == VSA_OK);
    append(2400);

    CHECK(max_step(left.data(), left.size()) < natural_step * 1.5);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
}

TEST_CASE("voice, bus and master gains multiply; the limiter holds -1 dBTP") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(0.5f), 1, 48000);
    const vsa_voice v = e.voice(asset, 0.5f, true, 1.0f, VSA_BUS_AMBIENT);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);

    const auto level = [&] {
        const auto left = channel(e.render(9600), 2, 0);
        return fit_sine(left.data() + 4800, 4800, 480.0, 48000.0).amplitude;
    };
    CHECK(level() == doctest::Approx(0.25 * kMonoPan).epsilon(0.001));

    REQUIRE(vsa_bus_set_gain(e.engine, VSA_BUS_AMBIENT, 0.5f) == VSA_OK);
    CHECK(level() == doctest::Approx(0.125 * kMonoPan).epsilon(0.001));
    REQUIRE(vsa_bus_set_gain(e.engine, VSA_BUS_SOUND, 0.0f) == VSA_OK);  // another bus: no effect
    CHECK(level() == doctest::Approx(0.125 * kMonoPan).epsilon(0.001));
    REQUIRE(vsa_engine_set_master_gain(e.engine, 2.0f) == VSA_OK);
    CHECK(level() == doctest::Approx(0.25 * kMonoPan).epsilon(0.001));
    REQUIRE(vsa_bus_set_gain(e.engine, VSA_BUS_AMBIENT, 0.0f) == VSA_OK);
    CHECK(level() < 1e-6);

    // Push well past full scale.
    REQUIRE(vsa_bus_set_gain(e.engine, VSA_BUS_AMBIENT, 1.0f) == VSA_OK);
    REQUIRE(vsa_engine_set_master_gain(e.engine, 16.0f) == VSA_OK);
    e.render(4800);
    (void)e.stats();  // reset the "since last read" figures
    const auto loud = e.render(48000);
    CHECK(peak(loud) <= 0.8913 + 1e-4);
    CHECK(e.stats().limiter_peak_reduction_db < -6.0f);
}

TEST_CASE("stereo assets keep their channels; mono is centred at -3 dB") {
    OfflineEngine e;
    std::vector<float> stereo = sine(480.0, 48000.0, 4800, 0.5f, 2);
    for (std::size_t i = 1; i < stereo.size(); i += 2) {
        stereo[i] = 0.0f;  // right channel silent
    }
    const AssetPtr asset = e.pcm(stereo, 2, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto out = e.render(9600);
    const auto left = channel(out, 2, 0);
    const auto right = channel(out, 2, 1);
    CHECK(fit_sine(left.data() + 1000, 8000, 480.0, 48000.0).amplitude == doctest::Approx(0.5).epsilon(0.001));
    CHECK(peak(right) < 1e-6);
}

TEST_CASE("invalid voice arguments are rejected") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = asset.get();
    desc.gain = 1.0f;
    desc.pitch = 1.0f;
    vsa_voice v = 0;

    desc.bus = 7;
    CHECK(vsa_voice_create(e.engine, &desc, &v) == VSA_ERROR_INVALID_ARGUMENT);
    desc.bus = VSA_BUS_MUSIC;
    desc.pitch = 0.0f;
    CHECK(vsa_voice_create(e.engine, &desc, &v) == VSA_ERROR_INVALID_ARGUMENT);
    desc.pitch = 1.0f;
    desc.gain = std::nanf("");
    CHECK(vsa_voice_create(e.engine, &desc, &v) == VSA_ERROR_INVALID_ARGUMENT);
    desc.gain = 1.0f;
    desc.asset = nullptr;
    CHECK(vsa_voice_create(e.engine, &desc, &v) == VSA_ERROR_INVALID_ARGUMENT);
    desc.asset = asset.get();
    desc.struct_size = 8;
    CHECK(vsa_voice_create(e.engine, &desc, &v) == VSA_ERROR_ABI_MISMATCH);

    const vsa_voice ok = e.voice(asset);
    CHECK(vsa_voice_set_gain(e.engine, ok, -1.0f) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_set_pitch(e.engine, ok, 9.0f) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_seek(e.engine, ok, -1.0) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_fade(e.engine, ok, 0.5f, 1.0f, 0x80, 0) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_bus_set_gain(e.engine, VSA_BUS_COUNT, 1.0f) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_start(nullptr, ok) == VSA_ERROR_INVALID_ARGUMENT);
    // Seeks past the end clamp to the end.
    REQUIRE(vsa_voice_seek(e.engine, ok, 99.0) == VSA_OK);
    CHECK(e.status(ok).position_seconds == doctest::Approx(0.1));
}

TEST_CASE("voice slots are a fixed capacity and are recycled after release") {
    auto config = make_config(VSA_RAY_TRACER_STEAM);
    config.max_voices = 3;
    OfflineEngine e(config);
    const AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    std::vector<vsa_voice> voices{e.voice(asset), e.voice(asset), e.voice(asset)};

    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = asset.get();
    desc.gain = 1.0f;
    desc.pitch = 1.0f;
    vsa_voice extra = 0;
    CHECK(vsa_voice_create(e.engine, &desc, &extra) == VSA_ERROR_CAPACITY);

    REQUIRE(vsa_voice_release(e.engine, voices[1]) == VSA_OK);
    e.render(256);  // the render thread retires the slot; the worker returns it shortly after
    vsa_result result = VSA_ERROR_CAPACITY;
    for (int i = 0; i < 500 && result == VSA_ERROR_CAPACITY; ++i) {
        result = vsa_voice_create(e.engine, &desc, &extra);
        if (result == VSA_ERROR_CAPACITY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    REQUIRE(result == VSA_OK);
    CHECK(extra != voices[1]);  // same slot, new generation
    CHECK((extra & 0xFFFFFFFFu) == (voices[1] & 0xFFFFFFFFu));
    CHECK(e.stats().allocated_voices == 3);
}

TEST_CASE("an asset outlives its creator's reference while voices use it") {
    OfflineEngine e;
    AssetPtr asset = e.pcm(loopable_sine(), 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    asset.reset();  // creator's reference gone; the voice keeps the asset alive
    const auto left = channel(e.render(9600), 2, 0);
    CHECK(fit_sine(left.data() + 1000, 8000, 480.0, 48000.0).amplitude == doctest::Approx(0.5 * kMonoPan).epsilon(0.001));
    REQUIRE(vsa_voice_release(e.engine, v) == VSA_OK);
    e.render(256);
}
