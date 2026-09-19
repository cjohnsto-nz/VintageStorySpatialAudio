// Positional voices: Steam Audio direct effect (distance, air absorption) + binaural/panning,
// the listener pose, virtualisation and the effect pool.

#include "engine_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace vsa_test;

namespace {

// 480 Hz sine, whole periods, loops seamlessly.
std::vector<float> tone(double frequency = 480.0, float amplitude = 0.5f) {
    return sine(frequency, 48000.0, 4800, amplitude);
}

// For direction tests: at 3 kHz a head gives a large level difference between the ears
// (at a few hundred Hz it is only ~5 dB for a source at 90 degrees; timing dominates there).
std::vector<float> directional_tone() { return tone(3000.0); }

struct Ears {
    double left_db;
    double right_db;
    [[nodiscard]] double balance() const { return right_db - left_db; }  // > 0: louder on the right
};

/// Renders `frames` (after skipping the first `settle` frames) and returns per-ear RMS.
Ears listen(OfflineEngine& e, std::size_t frames = 24000, std::size_t settle = 4800) {
    e.render(settle);
    const auto out = e.render(frames);
    return {to_db(rms(channel(out, 2, 0))), to_db(rms(channel(out, 2, 1)))};
}

double tone_level_db(const std::vector<float>& interleaved, uint32_t ch, double frequency) {
    const auto x = channel(interleaved, 2, ch);
    return to_db(fit_sine(x.data(), x.size(), frequency, 48000.0).amplitude);
}

void set_mode(OfflineEngine& e, vsa_render_mode mode) { REQUIRE(vsa_engine_set_render_mode(e.engine, mode) == VSA_OK); }

}  // namespace

TEST_CASE("a source on the right is louder in the right channel, in both render modes") {
    for (const vsa_render_mode mode : {VSA_RENDER_HEADPHONES, VSA_RENDER_SPEAKERS}) {
        CAPTURE(static_cast<int>(mode));
        OfflineEngine e;
        set_mode(e, mode);
        const AssetPtr asset = e.pcm(directional_tone(), 1, 48000);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 3.0f, 0.0f, 0.0f);  // listener at 0, facing -z
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const Ears right = listen(e);
        CHECK(right.balance() > 6.0);

        REQUIRE(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, -3.0f, 0.0f, 0.0f) == VSA_OK);
        const Ears left = listen(e);
        CHECK(left.balance() < -6.0);
        CHECK(e.stats().real_voices == 1);
    }
}

TEST_CASE("turning the listener around swaps the ears") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(directional_tone(), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 3.0f, 0.0f, 0.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    CHECK(listen(e).balance() > 6.0);
    e.listener(0, 0, 0, 0, 0, 1);  // facing +z: +x is now on the left
    CHECK(listen(e).balance() < -6.0);
    e.listener(10, 0, 0, 0, 0, -1);  // walked past it (facing -z again): the source is now on the left
    CHECK(listen(e).balance() < -6.0);
}

TEST_CASE("distance attenuation follows 1/r beyond the minimum distance") {
    const auto level_at = [](float distance, float min_distance) {
        OfflineEngine e;
        const AssetPtr asset = e.pcm(tone(), 1, 48000);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 0.0f, 0.0f, -distance, min_distance);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const Ears ears = listen(e);
        return (ears.left_db + ears.right_db) / 2.0;
    };
    // x4 distance = -12 dB (air absorption is negligible at 480 Hz over these distances).
    CHECK(level_at(2.0f, 1.0f) - level_at(8.0f, 1.0f) == doctest::Approx(12.04).epsilon(0.03));
    CHECK(level_at(16.0f, 4.0f) - level_at(32.0f, 4.0f) == doctest::Approx(6.02).epsilon(0.05));
    // Inside the minimum distance the level no longer rises.
    CHECK(std::abs(level_at(0.6f, 2.0f) - level_at(2.0f, 2.0f)) < 0.5);
}

TEST_CASE("air absorption darkens distant sources") {
    const auto high_minus_low = [](float distance) {
        OfflineEngine e;
        std::vector<float> two_tones = tone(480.0, 0.3f);
        const auto high = tone(12000.0, 0.3f);
        for (std::size_t i = 0; i < two_tones.size(); ++i) {
            two_tones[i] += high[i];
        }
        const AssetPtr asset = e.pcm(two_tones, 1, 48000);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 0.0f, 0.0f, -distance, distance);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        e.render(4800);
        set_mode(e, VSA_RENDER_SPEAKERS);  // plain panning: no HRTF colouring of the comparison
        e.render(4800);
        const auto out = e.render(24000);
        return tone_level_db(out, 0, 12000.0) - tone_level_db(out, 0, 480.0);
    };
    // Steam Audio's high band: exp(-0.0182 d) -> about -8 dB more at 100 m than at 1 m.
    CHECK(high_minus_low(1.0f) - high_minus_low(100.0f) > 5.0);
}

TEST_CASE("listener-relative voices stay put when the listener turns") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(directional_tone(), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_LISTENER, -3.0f, 0.0f, 0.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    CHECK(listen(e).balance() < -6.0);
    e.listener(50, 20, 7, 0, 0, 1);
    CHECK(listen(e).balance() < -6.0);
}

TEST_CASE("a source at the listener's head is centred") {
    OfflineEngine e;
    e.listener(5, 5, 5, 1, 0, 0);
    const AssetPtr asset = e.pcm(directional_tone(), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 5.0f, 5.0f, 5.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    CHECK(std::abs(listen(e).balance()) < 1.0);
}

TEST_CASE("inaudible voices go virtual, keep time and come back") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(sine(480.0, 48000.0, 48000), 1, 48000);  // 1 s one-shot
    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = asset.get();
    desc.gain = 1.0f;
    desc.pitch = 1.0f;
    desc.spatial = VSA_SPATIAL_WORLD;
    desc.position[2] = -10000.0f;  // 1/10000 = -80 dB
    vsa_voice v = 0;
    REQUIRE(vsa_voice_create(e.engine, &desc, &v) == VSA_OK);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);

    const auto far = e.render(24000);
    CHECK(peak(far) == 0.0);
    CHECK(e.stats().virtual_voices == 1);
    CHECK(e.stats().real_voices == 0);
    CHECK(e.status(v).position_seconds == doctest::Approx(0.5).epsilon(0.02));

    REQUIRE(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, 0.0f, 0.0f, -2.0f) == VSA_OK);
    const auto near = e.render(9600);
    CHECK(to_db(rms(channel(near, 2, 0))) > -30.0);
    CHECK(e.stats().virtual_voices == 0);
    CHECK(e.stats().real_voices == 1);

    // Back out of range: it still ends on time, having played 1 s in total.
    REQUIRE(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, 0.0f, 0.0f, -10000.0f) == VSA_OK);
    e.render(14400);
    CHECK(e.status(v).state == VSA_VOICE_STOPPED);
    bool ended = false;
    for (const vsa_event& event : e.events()) {
        ended = ended || event.type == VSA_EVENT_VOICE_ENDED;
    }
    CHECK(ended);
}

TEST_CASE("when the effect pool is full, the loudest positional voices are rendered") {
    auto config = make_config(VSA_RAY_TRACER_STEAM);
    config.max_real_voices = 2;
    OfflineEngine e(config);
    set_mode(e, VSA_RENDER_SPEAKERS);
    const AssetPtr low = e.pcm(tone(300.0), 1, 48000);
    const AssetPtr mid = e.pcm(tone(500.0), 1, 48000);
    const AssetPtr high = e.pcm(tone(700.0), 1, 48000);
    const vsa_voice near = e.positioned(low, VSA_SPATIAL_WORLD, 0, 0, -2);
    const vsa_voice middle = e.positioned(mid, VSA_SPATIAL_WORLD, 0, 0, -4);
    const vsa_voice distant = e.positioned(high, VSA_SPATIAL_WORLD, 0, 0, -50);
    for (const vsa_voice v : {near, middle, distant}) {
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    }
    e.render(4800);
    auto out = e.render(24000);
    CHECK(e.stats().real_voices == 2);
    CHECK(e.stats().virtual_voices == 1);
    CHECK(tone_level_db(out, 0, 300.0) > -30.0);
    CHECK(tone_level_db(out, 0, 500.0) > -30.0);
    CHECK(tone_level_db(out, 0, 700.0) < -100.0);

    // The distant voice comes close: it takes the set of the now quietest voice.
    REQUIRE(vsa_voice_set_position(e.engine, distant, VSA_SPATIAL_WORLD, 0, 0, -1) == VSA_OK);
    e.render(4800);
    out = e.render(24000);
    CHECK(e.stats().real_voices == 2);
    CHECK(tone_level_db(out, 0, 700.0) > -30.0);
    CHECK(tone_level_db(out, 0, 500.0) < -100.0);
    CHECK(tone_level_db(out, 0, 300.0) > -30.0);
}

TEST_CASE("the low-pass damps high frequencies like OpenAL's EFX filter") {
    OfflineEngine e;
    std::vector<float> two_tones = tone(200.0, 0.3f);
    const auto high = tone(12000.0, 0.3f);
    for (std::size_t i = 0; i < two_tones.size(); ++i) {
        two_tones[i] += high[i];
    }
    const AssetPtr asset = e.pcm(two_tones, 1, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true);  // not positioned: the filter applies to every voice
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(4800);
    const auto open = e.render(24000);
    REQUIRE(vsa_voice_set_lowpass(e.engine, v, 0.06f) == VSA_OK);  // the game's underwater value
    e.render(4800);
    const auto damped = e.render(24000);
    // A shelf reaching 0.06 (-24.4 dB) well above 5 kHz; the lows are untouched.
    CHECK(tone_level_db(damped, 0, 12000.0) - tone_level_db(open, 0, 12000.0) == doctest::Approx(-24.4).epsilon(0.1));
    CHECK(std::abs(tone_level_db(damped, 0, 200.0) - tone_level_db(open, 0, 200.0)) < 0.3);

    REQUIRE(vsa_voice_set_lowpass(e.engine, v, 1.0f) == VSA_OK);
    e.render(4800);
    const auto restored = e.render(24000);
    CHECK(std::abs(tone_level_db(restored, 0, 12000.0) - tone_level_db(open, 0, 12000.0)) < 0.1);
}

TEST_CASE("sources that jump in distance every block do not zipper") {
    OfflineEngine e;
    set_mode(e, VSA_RENDER_SPEAKERS);  // straight ahead: equal panning, isolates the distance gain
    const AssetPtr asset = e.pcm(tone(100.0, 0.5f), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 0, 0, -1);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(2560);
    std::vector<float> left;
    for (int i = 0; i < 40; ++i) {
        // 1 m <-> 3 m: a 9.5 dB jump each block.
        REQUIRE(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, 0, 0, i % 2 == 0 ? -3.0f : -1.0f) == VSA_OK);
        const auto block = channel(e.render(256), 2, 0);
        left.insert(left.end(), block.begin(), block.end());
    }
    // The signal's own largest step is about 2 pi f / fs * amplitude; an unsmoothed gain jump would
    // add up to (1 - 1/3) * amplitude in one sample.
    const double amplitude = peak(left);
    const double natural = 2.0 * std::numbers::pi * 100.0 / 48000.0 * amplitude;
    CHECK(max_step(left.data(), left.size()) < natural * 2.0);
}

namespace {

/// RMS (dB) per output channel of a positional noise source, on an offline output of `channels`.
std::vector<double> speaker_levels(uint32_t channels, vsa_render_mode mode, float x, float y, float z) {
    OfflineEngine e;
    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = VSA_OUTPUT_NONE;
    desc.channels = channels;
    REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
    set_mode(e, mode);
    // Broadband noise: panning is frequency independent, and the HRTF colours a tone unevenly.
    std::vector<float> noise(48000);
    uint32_t seed = 12345;
    for (float& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    const AssetPtr asset = e.pcm(noise, 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, x, y, z);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    std::vector<float> out(24000 * channels);
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), 4800) == VSA_OK);  // settle
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), 24000) == VSA_OK);
    std::vector<double> levels;
    for (uint32_t c = 0; c < channels; ++c) {
        levels.push_back(to_db(rms(channel(out, channels, c))));
    }
    return levels;
}

double power_sum_db(const std::vector<double>& levels, std::initializer_list<std::size_t> indices) {
    double power = 0.0;
    for (const std::size_t i : indices) {
        power += std::pow(10.0, levels[i] / 10.0);
    }
    return 10.0 * std::log10(std::max(power, 1e-30));
}

// Offline outputs use the engine's (Steam Audio's) order.
enum : std::size_t { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5, SL = 6, SR = 7 };

}  // namespace

TEST_CASE("5.1: front, behind and side sources reach the right speakers; the LFE stays silent") {
    const auto front = speaker_levels(6, VSA_RENDER_SPEAKERS, 0, 0, -3);
    CHECK(front[FC] > power_sum_db(front, {BL, BR}) + 12.0);
    CHECK(front[LFE] < -120.0);

    const auto behind = speaker_levels(6, VSA_RENDER_SPEAKERS, 0, 0, 3);
    CHECK(power_sum_db(behind, {BL, BR}) > power_sum_db(behind, {FL, FR, FC}) + 10.0);

    const auto left = speaker_levels(6, VSA_RENDER_SPEAKERS, -3, 0, 0);
    CHECK(power_sum_db(left, {FL, BL}) > power_sum_db(left, {FR, BR}) + 10.0);
}

TEST_CASE("7.1: a source to the side comes from the side speaker") {
    const auto left = speaker_levels(8, VSA_RENDER_SPEAKERS, -3, 0, 0);
    const double strongest = *std::max_element(left.begin(), left.end());
    CHECK(left[SL] == doctest::Approx(strongest));
    CHECK(left[SL] > power_sum_db(left, {FR, BR, SR}) + 10.0);
    CHECK(left[LFE] < -120.0);
}

TEST_CASE("quad: a source behind comes from the rear pair") {
    const auto behind = speaker_levels(4, VSA_RENDER_SPEAKERS, 0, 0, 3);
    CHECK(power_sum_db(behind, {2, 3}) > power_sum_db(behind, {0, 1}) + 10.0);
}

TEST_CASE("binaural rendering on a surround output uses the front pair only") {
    const auto levels = speaker_levels(6, VSA_RENDER_HEADPHONES, 3, 0, 0);
    CHECK(levels[FR] > -40.0);
    for (const std::size_t c : {FC, LFE, BL, BR}) {
        CHECK(levels[c] < -120.0);
    }
}

TEST_CASE("invalid spatial arguments are rejected") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(tone(), 1, 48000);
    const vsa_voice v = e.voice(asset);
    CHECK(vsa_voice_set_position(e.engine, v, 3, 0, 0, 0) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, std::nanf(""), 0, 0) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_voice_set_lowpass(e.engine, v, 1.5f) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_engine_set_render_mode(e.engine, 7) == VSA_ERROR_INVALID_ARGUMENT);

    vsa_listener l{};
    l.struct_size = sizeof l;
    l.forward[2] = -1.0f;  // up left at zero
    CHECK(vsa_listener_set(e.engine, &l) == VSA_ERROR_INVALID_ARGUMENT);
    l.up[2] = 1.0f;  // parallel to forward
    CHECK(vsa_listener_set(e.engine, &l) == VSA_ERROR_INVALID_ARGUMENT);
    l.struct_size = 12;
    CHECK(vsa_listener_set(e.engine, &l) == VSA_ERROR_ABI_MISMATCH);

    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = asset.get();
    desc.gain = 1.0f;
    desc.pitch = 1.0f;
    desc.spatial = 9;
    vsa_voice out = 0;
    CHECK(vsa_voice_create(e.engine, &desc, &out) == VSA_ERROR_INVALID_ARGUMENT);
    desc.spatial = VSA_SPATIAL_WORLD;
    desc.min_distance = -1.0f;
    CHECK(vsa_voice_create(e.engine, &desc, &out) == VSA_ERROR_INVALID_ARGUMENT);
}
