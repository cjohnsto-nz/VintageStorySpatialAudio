// Beds (unpositioned sounds of more than two channels): which speaker each channel is for, and
// how it reaches the output's speakers.

#include "audio/bed_panner.hpp"
#include "audio/source_layout.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using vsa::BedPanner;
using vsa::SourceChannel;

namespace {

enum : std::size_t { FL = 0, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR };

std::array<float, 12> gains(const BedPanner& panner, const SourceChannel& channel) {
    std::array<float, 12> g{};
    panner.gains(channel, g.data());
    return g;
}

double power(const std::array<float, 12>& g) {
    double sum = 0.0;
    for (const float x : g) {
        sum += static_cast<double>(x) * x;
    }
    return sum;
}

}  // namespace

TEST_CASE("Ogg Vorbis channel order: 5.1 is FL C FR surrounds LFE, with the surrounds at 110 degrees") {
    const auto five_one = vsa::vorbis_layout(6);
    CHECK(five_one[0].azimuth == doctest::Approx(-30.0f));
    CHECK(five_one[1].azimuth == doctest::Approx(0.0f));
    CHECK(five_one[2].azimuth == doctest::Approx(30.0f));
    CHECK(five_one[3].azimuth == doctest::Approx(-110.0f));
    CHECK(five_one[4].azimuth == doctest::Approx(110.0f));
    CHECK(five_one[5].lfe);

    const auto seven_one = vsa::vorbis_layout(8);
    CHECK(seven_one[3].azimuth == doctest::Approx(-90.0f));
    CHECK(seven_one[5].azimuth == doctest::Approx(-150.0f));
    CHECK(seven_one[7].lfe);
}

TEST_CASE("WAV channel order follows the speaker mask, or the default order without one") {
    const auto five_one = vsa::wave_layout(6, 0);  // FL FR FC LFE BL BR
    CHECK(five_one[1].azimuth == doctest::Approx(30.0f));
    CHECK(five_one[2].azimuth == doctest::Approx(0.0f));
    CHECK(five_one[3].lfe);
    CHECK(five_one[4].azimuth == doctest::Approx(-110.0f));

    const auto side = vsa::wave_layout(6, 0x60F);  // 5.1 with side surrounds: still at 110
    CHECK(side[4].azimuth == doctest::Approx(-110.0f));

    const auto seven_one = vsa::wave_layout(8, 0x63F);  // both pairs: backs at 150, sides at 90
    CHECK(seven_one[4].azimuth == doctest::Approx(-150.0f));
    CHECK(seven_one[6].azimuth == doctest::Approx(-90.0f));

    const auto heights = vsa::wave_layout(4, 0x1000 | 0x4000 | 0x8000 | 0x20000);  // the four tops
    CHECK(heights[0].elevation == doctest::Approx(45.0f));
    CHECK(heights[3].azimuth == doctest::Approx(135.0f));
}

TEST_CASE("5.1 on 5.1 speakers: every channel plays from its own speaker alone") {
    const BedPanner panner(6);
    const auto layout = vsa::vorbis_layout(6);
    const std::array<std::size_t, 6> speaker = {FL, FC, FR, BL, BR, LFE};
    for (std::size_t c = 0; c < speaker.size(); ++c) {
        CAPTURE(c);
        const auto g = gains(panner, layout[c]);
        CHECK(g[speaker[c]] == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(power(g) == doctest::Approx(1.0).epsilon(1e-4));
    }
}

TEST_CASE("5.1 on 7.1.4 speakers: the surrounds sit between the sides and backs, nearer the sides") {
    const BedPanner panner(12);
    const auto layout = vsa::vorbis_layout(6);
    const auto left = gains(panner, layout[3]);
    CHECK(left[SL] > left[BL]);
    CHECK(left[BL] > 0.1f);
    CHECK(left[SL] * left[SL] + left[BL] * left[BL] == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(gains(panner, layout[1])[FC] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(gains(panner, layout[5])[LFE] == doctest::Approx(1.0f));
    CHECK(power(gains(panner, SourceChannel{-45.0f, 45.0f})) == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("5.1 on stereo: fronts stay on their side, the centre in the middle, surrounds folded 3 dB down") {
    const BedPanner panner(2);
    const auto layout = vsa::vorbis_layout(6);
    const auto fl = gains(panner, layout[0]);
    CHECK(fl[0] == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(std::abs(fl[1]) < 1e-5f);
    const auto centre = gains(panner, layout[1]);
    CHECK(centre[0] == doctest::Approx(centre[1]));
    const auto surround_right = gains(panner, layout[4]);
    CHECK(surround_right[1] == doctest::Approx(0.70710678f).epsilon(1e-4));
    CHECK(std::abs(surround_right[0]) < 1e-5f);
    const auto lfe = gains(panner, layout[5]);  // no LFE speaker: the front pair
    CHECK(lfe[0] == doctest::Approx(lfe[1]));
    CHECK(lfe[0] > 0.5f);
}
