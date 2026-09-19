// Beds: unpositioned sounds of more than two channels (a weather mod's 5.1 rain). Each channel
// plays from its speaker, head-locked; on headphones binaurally.

#include "engine_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vsa_test;

namespace {

// Offline outputs use the engine's order.
enum : std::size_t { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5, SL = 6, SR = 7 };

/// Interleaved noise with signal in channel `active` only.
std::vector<float> noise_in(uint32_t channels, uint32_t active, std::size_t frames = 48000) {
    std::vector<float> out(frames * channels, 0.0f);
    uint32_t seed = 12345;
    for (std::size_t j = 0; j < frames; ++j) {
        seed = seed * 1664525u + 1013904223u;
        out[j * channels + active] = 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return out;
}

void open_offline(OfflineEngine& e, uint32_t channels) {
    vsa_output_desc desc{};
    desc.struct_size = sizeof desc;
    desc.kind = VSA_OUTPUT_NONE;
    desc.channels = channels;
    REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
}

/// RMS (dB) per output channel after the voice has played for a while.
std::vector<double> levels(OfflineEngine& e, uint32_t channels) {
    std::vector<float> out(24000 * channels);
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), 4800) == VSA_OK);  // settle
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), 24000) == VSA_OK);
    std::vector<double> db;
    for (uint32_t c = 0; c < channels; ++c) {
        db.push_back(to_db(rms(channel(out, channels, c))));
    }
    return db;
}

/// Levels on an output of `outputs` channels of a looping bed (PCM, WAV order) of
/// `source_channels` with signal in `active` only.
std::vector<double> bed_levels(uint32_t outputs, vsa_render_mode mode, uint32_t source_channels, uint32_t active) {
    OfflineEngine e;
    open_offline(e, outputs);
    REQUIRE(vsa_engine_set_render_mode(e.engine, mode) == VSA_OK);
    const AssetPtr asset = e.pcm(noise_in(source_channels, active), source_channels, 48000);
    const vsa_voice v = e.voice(asset, 1.0f, true, 1.0f, VSA_BUS_WEATHER);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    return levels(e, outputs);
}

double loudest_other(const std::vector<double>& db, std::size_t except) {
    double loudest = -300.0;
    for (std::size_t c = 0; c < db.size(); ++c) {
        if (c != except) {
            loudest = std::max(loudest, db[c]);
        }
    }
    return loudest;
}

}  // namespace

TEST_CASE("beds: a 5.1 bed on 5.1 speakers plays each channel from its own speaker, the LFE included") {
    // PCM is in WAV's order (FL FR FC LFE BL BR), which is the engine's 5.1 order.
    for (uint32_t active = 0; active < 6; ++active) {
        CAPTURE(active);
        const auto db = bed_levels(6, VSA_RENDER_SPEAKERS, 6, active);
        CHECK(db[active] > -30.0);
        CHECK(db[active] > loudest_other(db, active) + 60.0);
    }
}

TEST_CASE("beds: on 7.1.4 a 5.1 surround sits between the side and back speakers, nearer the side") {
    const auto db = bed_levels(12, VSA_RENDER_SPEAKERS, 6, 4);  // back left, at 110 degrees
    CHECK(db[SL] > db[BL]);
    CHECK(db[BL] > db[SL] - 12.0);
    CHECK(std::max(db[SL], db[BL]) > db[FL] + 60.0);
    CHECK(db[SL] > db[SR] + 60.0);
}

TEST_CASE("beds: on stereo the surrounds fold to their side 3 dB down, the centre to both") {
    const auto front_left = bed_levels(2, VSA_RENDER_SPEAKERS, 6, 0);
    CHECK(front_left[0] > front_left[1] + 60.0);
    const auto surround_left = bed_levels(2, VSA_RENDER_SPEAKERS, 6, 4);
    CHECK(surround_left[0] == doctest::Approx(front_left[0] - 3.01).epsilon(0.01));
    const auto centre = bed_levels(2, VSA_RENDER_SPEAKERS, 6, 2);
    CHECK(centre[0] == doctest::Approx(centre[1]).epsilon(0.001));
}

TEST_CASE("beds: on headphones each channel is heard from its side, and the bed turns with the head") {
    for (const bool turned : {false, true}) {
        CAPTURE(turned);
        const auto heard = [&](uint32_t active) {
            OfflineEngine e;
            REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_HEADPHONES) == VSA_OK);
            if (turned) {
                e.listener(0, 0, 0, 1.0f, 0.0f, 0.0f);  // facing +x instead of -z
            }
            const AssetPtr asset = e.pcm(noise_in(6, active), 6, 48000);
            const vsa_voice v = e.voice(asset, 1.0f, true, 1.0f, VSA_BUS_WEATHER);
            REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
            const auto db = levels(e, 2);
            return db[1] - db[0];  // > 0: louder on the right
        };
        const double left_surround = heard(4);
        const double right_surround = heard(5);
        const double centre = heard(2);
        MESSAGE("balance: left surround " << left_surround << " dB, right " << right_surround << " dB, centre " << centre);
        CHECK(left_surround < -6.0);
        CHECK(right_surround > 6.0);
        CHECK(std::abs(centre) < 1.0);
    }
}

TEST_CASE("beds: a bed takes its bus gain, on speakers and on headphones") {
    for (const vsa_render_mode mode : {VSA_RENDER_SPEAKERS, VSA_RENDER_HEADPHONES}) {
        CAPTURE(static_cast<int>(mode));
        OfflineEngine e;
        open_offline(e, 6);
        REQUIRE(vsa_engine_set_render_mode(e.engine, mode) == VSA_OK);
        const AssetPtr asset = e.pcm(noise_in(6, 4), 6, 48000);
        const vsa_voice v = e.voice(asset, 1.0f, true, 1.0f, VSA_BUS_WEATHER);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const auto on = levels(e, 6);
        REQUIRE(vsa_bus_set_gain(e.engine, VSA_BUS_WEATHER, 0.0f) == VSA_OK);
        levels(e, 6);  // the bus gain ramps and the decoder's tail rings out
        const auto off = levels(e, 6);
        const double loudest_on = *std::max_element(on.begin(), on.end());
        const double loudest_off = *std::max_element(off.begin(), off.end());
        CHECK(loudest_on > -40.0);
        CHECK(loudest_off < -120.0);
    }
}

TEST_CASE("beds: a positioned sound of more channels is downmixed and heard from its place") {
    OfflineEngine e;
    REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_HEADPHONES) == VSA_OK);
    const AssetPtr asset = e.pcm(noise_in(6, 0), 6, 48000);  // front left only: position decides
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 3.0f, 0.0f, 0.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto db = levels(e, 2);
    CHECK(db[1] > db[0] + 6.0);
    CHECK(db[1] > -60.0);
}

TEST_CASE("beds: a streamed 5.1 Ogg Vorbis bed plays its channels in Vorbis order") {
    auto config = make_config(VSA_RAY_TRACER_STEAM);
    config.stream_threshold_ms = 100;
    OfflineEngine e(config);
    open_offline(e, 6);
    REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
    // Vorbis 5.1: FL C FR surround-left surround-right LFE. Signal in the centre and the right surround.
    std::vector<float> pcm = noise_in(6, 1);
    const std::vector<float> right = noise_in(6, 4);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] += right[i];
    }
    const AssetPtr asset = e.asset(ogg_file(pcm, 6, 48000));
    vsa_asset_info info{};
    info.struct_size = sizeof info;
    REQUIRE(vsa_asset_get_info(asset.get(), &info) == VSA_OK);
    CHECK(info.channels == 6);
    CHECK(info.storage == VSA_ASSET_STORAGE_STREAMED);
    const vsa_voice v = e.voice(asset, 1.0f, true, 1.0f, VSA_BUS_WEATHER);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto db = levels(e, 6);
    CHECK(db[FC] > -40.0);
    CHECK(db[BR] > -40.0);
    for (const std::size_t quiet : {FL, FR, LFE, BL}) {
        CAPTURE(quiet);
        CHECK(db[quiet] < std::min(db[FC], db[BR]) - 20.0);  // lossy coding leaks a little
    }
}
