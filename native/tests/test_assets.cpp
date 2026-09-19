// Asset creation through the C ABI: formats, storage selection and errors.

#include "engine_fixture.hpp"

#include <cstring>
#include <string>

using namespace vsa_test;

namespace {

vsa_asset_info info_of(const AssetPtr& asset) {
    vsa_asset_info info{};
    info.struct_size = sizeof info;
    REQUIRE(vsa_asset_get_info(asset.get(), &info) == VSA_OK);
    return info;
}

/// Plays a mono asset once at unit pitch and returns the left output channel. Renders whole
/// blocks so the next call starts on a block boundary (voices start at block boundaries).
std::vector<float> play(OfflineEngine& e, const AssetPtr& asset, std::size_t frames) {
    const vsa_voice v = e.voice(asset);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto left = channel(e.render((frames + 255) / 256 * 256), 2, 0);
    REQUIRE(vsa_voice_release(e.engine, v) == VSA_OK);
    return left;
}

vsa_result try_create(OfflineEngine& e, const void* data, std::size_t size, uint32_t format = VSA_ASSET_FORMAT_AUTO,
                      uint32_t storage = VSA_ASSET_STORAGE_AUTO) {
    vsa_asset_desc desc{};
    desc.struct_size = sizeof desc;
    desc.format = format;
    desc.storage = storage;
    desc.data = data;
    desc.size = size;
    vsa_asset* asset = nullptr;
    const vsa_result result = vsa_asset_create(e.engine, &desc, &asset);
    vsa_asset_release(asset);
    return result;
}

}  // namespace

TEST_CASE("raw PCM assets report their format") {
    OfflineEngine e;
    const AssetPtr asset = e.pcm(sine(440.0, 22050.0, 11025, 0.5f, 2), 2, 22050);
    const vsa_asset_info info = info_of(asset);
    CHECK(info.channels == 2);
    CHECK(info.sample_rate == 22050);
    CHECK(info.frames == 11025);
    CHECK(info.storage == VSA_ASSET_STORAGE_DECODED);
    CHECK(info.memory_bytes == 11025 * 2 * 2);
    CHECK(info.duration_seconds == doctest::Approx(0.5));
}

TEST_CASE("WAV files decode in every supported sample format") {
    OfflineEngine e;
    const auto source = sine(480.0, 48000.0, 4800, 0.5f);
    const struct {
        uint32_t bits;
        bool floating;
        double max_error;
    } formats[] = {{8, false, 1.0 / 64}, {16, false, 1e-4}, {24, false, 1e-4}, {32, false, 1e-4},
                   {32, true, 1e-4},     {64, true, 1e-4}};
    for (const auto& f : formats) {
        CAPTURE(f.bits);
        CAPTURE(f.floating);
        const AssetPtr asset = e.asset(wav_file(source, 1, 48000, f.bits, f.floating));
        const vsa_asset_info info = info_of(asset);
        CHECK(info.channels == 1);
        CHECK(info.sample_rate == 48000);
        CHECK(info.frames == 4800);

        const auto out = play(e, asset, 4800 + kLimiterLatency);
        double error = 0.0;
        for (std::size_t i = 0; i < 4800; ++i) {
            error = std::max(error, std::abs(static_cast<double>(out[i + kLimiterLatency]) -
                                             static_cast<double>(source[i]) * kMonoPan));
        }
        CHECK(error < f.max_error);
    }
}

TEST_CASE("Ogg Vorbis decodes to the encoded tone") {
    OfflineEngine e;
    const auto ogg = ogg_file(sine(1000.0, 44100.0, 44100, 0.5f, 2), 2, 44100);
    const AssetPtr asset = e.asset(ogg, VSA_ASSET_STORAGE_DECODED);
    const vsa_asset_info info = info_of(asset);
    CHECK(info.channels == 2);
    CHECK(info.sample_rate == 44100);
    CHECK(info.frames == 44100);
    CHECK(info.storage == VSA_ASSET_STORAGE_DECODED);

    const auto out = play(e, asset, 36000);
    const SineFit fit = fit_sine(out.data() + 4000, 30000, 1000.0, 48000.0);
    CHECK(fit.amplitude == doctest::Approx(0.5).epsilon(0.01));
    CHECK(fit.thd_n_db() < -40.0);  // lossy codec
}

TEST_CASE("AUTO storage streams Ogg assets longer than the threshold") {
    auto config = make_config(VSA_RAY_TRACER_STEAM);
    config.stream_threshold_ms = 500;
    OfflineEngine e(config);
    const auto short_ogg = ogg_file(sine(500.0, 44100.0, 11025), 1, 44100);  // 0.25 s
    const auto long_ogg = ogg_file(sine(500.0, 44100.0, 44100), 1, 44100);   // 1 s
    CHECK(info_of(e.asset(short_ogg)).storage == VSA_ASSET_STORAGE_DECODED);
    const vsa_asset_info streamed = info_of(e.asset(long_ogg));
    CHECK(streamed.storage == VSA_ASSET_STORAGE_STREAMED);
    CHECK(streamed.frames == 44100);
    CHECK(streamed.memory_bytes == long_ogg.size());
    // WAV is always decoded.
    CHECK(info_of(e.asset(wav_file(sine(500.0, 44100.0, 88200), 1, 44100, 16))).storage == VSA_ASSET_STORAGE_DECODED);
}

TEST_CASE("bad asset data is rejected with a specific error") {
    OfflineEngine e;
    const std::string garbage = "definitely not audio";
    CHECK(try_create(e, garbage.data(), garbage.size()) == VSA_ERROR_DECODE);
    CHECK(std::string(vsa_get_last_error()).find("unrecognised") != std::string::npos);
    CHECK(try_create(e, garbage.data(), 0) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(try_create(e, nullptr, 16) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(try_create(e, garbage.data(), garbage.size(), 17) == VSA_ERROR_INVALID_ARGUMENT);

    const auto surround = wav_file(sine(440.0, 48000.0, 480, 0.5f, 6), 6, 48000, 16);
    CHECK(try_create(e, surround.data(), surround.size()) == VSA_ERROR_UNSUPPORTED);
    CHECK(std::string(vsa_get_last_error()).find("6 channels") != std::string::npos);

    const auto wav = wav_file(sine(440.0, 48000.0, 480), 1, 48000, 16);
    CHECK(try_create(e, wav.data(), wav.size(), VSA_ASSET_FORMAT_AUTO, VSA_ASSET_STORAGE_STREAMED) ==
          VSA_ERROR_UNSUPPORTED);
    CHECK(try_create(e, wav.data(), 20) == VSA_ERROR_DECODE);  // truncated before the fmt chunk

    const auto ogg = ogg_file(sine(440.0, 44100.0, 4410), 1, 44100);
    std::vector<uint8_t> corrupt(ogg.begin(), ogg.begin() + 40);
    CHECK(try_create(e, corrupt.data(), corrupt.size()) == VSA_ERROR_DECODE);

    // Raw PCM needs a format.
    const int16_t pcm[4] = {0, 1, 2, 3};
    vsa_asset_desc desc{};
    desc.struct_size = sizeof desc;
    desc.format = VSA_ASSET_FORMAT_PCM_S16;
    desc.data = pcm;
    desc.size = sizeof pcm;
    vsa_asset* asset = nullptr;
    desc.pcm_channels = 3;
    desc.pcm_sample_rate = 48000;
    CHECK(vsa_asset_create(e.engine, &desc, &asset) == VSA_ERROR_UNSUPPORTED);
    desc.pcm_channels = 1;
    desc.pcm_sample_rate = 0;
    CHECK(vsa_asset_create(e.engine, &desc, &asset) == VSA_ERROR_INVALID_ARGUMENT);
    desc.pcm_sample_rate = 48000;
    desc.size = 3;
    CHECK(vsa_asset_create(e.engine, &desc, &asset) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(asset == nullptr);

    vsa_asset_release(nullptr);  // accepted
}
