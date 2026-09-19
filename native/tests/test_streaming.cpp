// Streamed Ogg assets: decode-ahead on the worker, looping, seeking and ending.

#include "engine_fixture.hpp"

using namespace vsa_test;

namespace {

struct StreamFixture {
    OfflineEngine e;
    std::vector<uint8_t> ogg = ogg_file(sine(441.0, 44100.0, 3 * 44100, 0.5f, 2), 2, 44100);  // 3 s
    AssetPtr streamed = e.asset(ogg, VSA_ASSET_STORAGE_STREAMED);
    AssetPtr decoded = e.asset(ogg, VSA_ASSET_STORAGE_DECODED);
};

bool has_event(const std::vector<vsa_event>& events, vsa_event_type type) {
    for (const vsa_event& event : events) {
        if (event.type == static_cast<uint32_t>(type)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a streamed asset sounds the same as the decoded one") {
    StreamFixture f;
    const vsa_voice a = f.e.voice(f.streamed);
    REQUIRE(vsa_voice_start(f.e.engine, a) == VSA_OK);
    const auto streamed = channel(f.e.render(96000), 2, 0);
    REQUIRE(vsa_voice_release(f.e.engine, a) == VSA_OK);
    f.e.render(512);

    const vsa_voice b = f.e.voice(f.decoded);
    REQUIRE(vsa_voice_start(f.e.engine, b) == VSA_OK);
    const auto decoded = channel(f.e.render(96000), 2, 0);

    // The decoded copy is quantised to 16 bits; otherwise they are the same samples.
    std::vector<float> difference(streamed.size());
    for (std::size_t i = 0; i < difference.size(); ++i) {
        difference[i] = streamed[i] - decoded[i];
    }
    CHECK(to_db(rms(difference) / rms(decoded)) < -80.0);
    CHECK(f.e.stats().stream_underruns == 0);
}

TEST_CASE("a streamed voice ends at the end of the stream and can restart") {
    StreamFixture f;
    const vsa_voice v = f.e.voice(f.streamed);
    REQUIRE(vsa_voice_start(f.e.engine, v) == VSA_OK);
    f.e.render(48000 * 3 + 9600);
    CHECK(f.e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(f.e.status(v).position_seconds == 0.0);
    CHECK(has_event(f.e.events(), VSA_EVENT_VOICE_ENDED));

    REQUIRE(vsa_voice_start(f.e.engine, v) == VSA_OK);
    const auto again = channel(f.e.render(24000), 2, 0);
    CHECK(fit_sine(again.data() + 4000, 16000, 441.0, 48000.0).amplitude == doctest::Approx(0.5).epsilon(0.02));
}

TEST_CASE("a looping streamed voice keeps playing past its length") {
    StreamFixture f;
    const vsa_voice v = f.e.voice(f.streamed, 1.0f, true);
    REQUIRE(vsa_voice_start(f.e.engine, v) == VSA_OK);
    const auto out = channel(f.e.render(48000 * 4), 2, 0);
    CHECK(f.e.status(v).state == VSA_VOICE_PLAYING);
    const double position = f.e.status(v).position_seconds;
    CHECK(position == doctest::Approx(1.0).epsilon(0.02));  // 4 s into a 3 s loop
    // Still the tone after the loop point.
    CHECK(fit_sine(out.data() + 48000 * 3 + 4800, 24000, 441.0, 48000.0).amplitude ==
          doctest::Approx(0.5).epsilon(0.02));
    CHECK(f.e.stats().stream_underruns == 0);
}

TEST_CASE("seeking a streamed voice") {
    StreamFixture f;
    const vsa_voice v = f.e.voice(f.streamed);
    REQUIRE(vsa_voice_start(f.e.engine, v) == VSA_OK);
    f.e.render(4800);
    REQUIRE(vsa_voice_seek(f.e.engine, v, 2.0) == VSA_OK);
    CHECK(f.e.status(v).position_seconds == doctest::Approx(2.0));
    f.e.render(24000);
    CHECK(f.e.status(v).position_seconds == doctest::Approx(2.5).epsilon(0.02));
    // 0.5 s left, then it ends.
    f.e.render(48000);
    CHECK(f.e.status(v).state == VSA_VOICE_STOPPED);
    CHECK(has_event(f.e.events(), VSA_EVENT_VOICE_ENDED));
}
