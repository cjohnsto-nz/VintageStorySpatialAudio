// The Ambisonic speaker decoder for the reflections (ADR 0009): plane waves decode at an even
// power from every direction the layout covers, towards the speakers in that direction, and the
// decoder follows the listener's head.

#include "audio/channel_layout.hpp"
#include "audio/speaker_decoder.hpp"
#include "dsp/spherical_harmonics.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using vsa::Speaker;
using vsa::SpeakerDecoder;

namespace {

double power(const std::vector<float>& gains) {
    double p = 0.0;
    for (const float g : gains) {
        p += static_cast<double>(g) * static_cast<double>(g);
    }
    return p;
}

/// Listener-space direction (+x right, +y up, -z ahead) from azimuth (clockwise from ahead) and
/// elevation, degrees.
std::array<float, 3> direction(double azimuth, double elevation) {
    const double az = azimuth * std::numbers::pi / 180.0;
    const double el = elevation * std::numbers::pi / 180.0;
    return {static_cast<float>(std::sin(az) * std::cos(el)), static_cast<float>(std::sin(el)),
            static_cast<float>(-std::cos(az) * std::cos(el))};
}

int channel_of(uint32_t channels, Speaker speaker) {
    const auto layout = vsa::steam_layout(channels);
    for (uint32_t c = 0; c < channels; ++c) {
        if (layout[c] == speaker) {
            return static_cast<int>(c);
        }
    }
    return -1;
}

int loudest(const std::vector<float>& gains) {
    return static_cast<int>(std::max_element(gains.begin(), gains.end()) - gains.begin());
}

}  // namespace

TEST_CASE("speaker decoder: even power around the ear-level ring, for every layout and order") {
    for (const uint32_t channels : {2u, 4u, 6u, 8u, 12u}) {
        for (const int order : {1, 2, 3}) {
            CAPTURE(channels);
            CAPTURE(order);
            const SpeakerDecoder decoder(channels, order);
            std::vector<float> gains(channels);
            double lo = 1e9;
            double hi = 0.0;
            for (int a = 0; a < 360; a += 5) {
                const auto d = direction(a, 0.0);
                decoder.plane_wave(d.data(), gains.data());
                const double p = power(gains);
                lo = std::min(lo, p);
                hi = std::max(hi, p);
            }
            const double spread_db = 10.0 * std::log10(hi / lo);
            MESSAGE(channels << " ch, order " << order << ": ear-level power spread " << spread_db << " dB");
            CHECK(spread_db < 3.0);
            if (channels >= 6) {
                const int lfe = channel_of(channels, Speaker::Lfe);
                REQUIRE(lfe >= 0);
                const auto d = direction(30.0, 10.0);
                decoder.plane_wave(d.data(), gains.data());
                CHECK(gains[static_cast<std::size_t>(lfe)] == 0.0f);
            }
        }
    }
}

TEST_CASE("speaker decoder: 7.1.4 has even power over the whole upper hemisphere") {
    const SpeakerDecoder decoder(12, 2);
    std::vector<float> gains(12);
    double lo = 1e9;
    double hi = 0.0;
    for (int e = 0; e <= 90; e += 15) {
        for (int a = 0; a < 360; a += 15) {
            const auto d = direction(a, e);
            decoder.plane_wave(d.data(), gains.data());
            lo = std::min(lo, power(gains));
            hi = std::max(hi, power(gains));
        }
    }
    const double spread_db = 10.0 * std::log10(hi / lo);
    MESSAGE("7.1.4 upper hemisphere power spread " << spread_db << " dB");
    CHECK(spread_db < 3.0);
}

TEST_CASE("speaker decoder: sound comes from the speakers in its direction") {
    SUBCASE("7.1.4") {
        const SpeakerDecoder decoder(12, 3);
        std::vector<float> gains(12);
        const auto front_left = direction(-30.0, 0.0);
        decoder.plane_wave(front_left.data(), gains.data());
        CHECK(loudest(gains) == channel_of(12, Speaker::FrontLeft));
        const auto back_right = direction(150.0, 0.0);
        decoder.plane_wave(back_right.data(), gains.data());
        CHECK(loudest(gains) == channel_of(12, Speaker::BackRight));
        const auto top_front_right = direction(45.0, 45.0);
        decoder.plane_wave(top_front_right.data(), gains.data());
        CHECK(loudest(gains) == channel_of(12, Speaker::TopFrontRight));
    }
    SUBCASE("stereo") {
        const SpeakerDecoder decoder(2, 2);
        std::vector<float> gains(2);
        const auto right = direction(90.0, 0.0);
        decoder.plane_wave(right.data(), gains.data());
        CHECK(gains[1] > 2.5f * std::abs(gains[0]));  // 8 dB: max-rE spreads a little
        const auto ahead = direction(0.0, 0.0);
        decoder.plane_wave(ahead.data(), gains.data());
        CHECK(static_cast<double>(gains[0]) == doctest::Approx(static_cast<double>(gains[1])).epsilon(0.01));
    }
}

TEST_CASE("speaker decoder: follows the listener's head") {
    // A plane wave from world east (+x); the listener faces east, so it is ahead of them.
    SpeakerDecoder decoder(12, 2);
    const float east[3] = {1.0f, 0.0f, 0.0f};
    float sh[vsa::dsp::kSh3Channels];
    vsa::dsp::sh_order3(east, sh);
    constexpr uint32_t kFrames = 64;
    std::vector<std::vector<float>> in(decoder.ambisonic_channels(), std::vector<float>(kFrames));
    std::vector<const float*> in_ptr;
    for (uint32_t c = 0; c < decoder.ambisonic_channels(); ++c) {
        std::fill(in[c].begin(), in[c].end(), sh[c]);
        in_ptr.push_back(in[c].data());
    }
    std::vector<std::vector<float>> out(12, std::vector<float>(kFrames, 0.0f));
    std::vector<float*> out_ptr;
    for (auto& o : out) {
        out_ptr.push_back(o.data());
    }
    const float right[3] = {0.0f, 0.0f, 1.0f};  // facing +x, right is +z
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float ahead[3] = {1.0f, 0.0f, 0.0f};
    decoder.decode(right, up, ahead, in_ptr.data(), out_ptr.data(), kFrames);
    std::vector<float> last(12);
    for (uint32_t s = 0; s < 12; ++s) {
        last[s] = out[s][kFrames - 1];
    }
    CHECK(loudest(last) == channel_of(12, Speaker::FrontCentre));
}

TEST_CASE("speaker decoder: left/right contrast of a plane wave from the left, per order (7.1.4)") {
    for (const int order : {1, 2, 3}) {
        const SpeakerDecoder decoder(12, order);
        std::vector<float> gains(12);
        const auto d = direction(-90.0, 0.0);
        decoder.plane_wave(d.data(), gains.data());
        double l = 0.0, r = 0.0;
        for (const int c : {0, 4, 6, 8, 10}) {
            const auto g = static_cast<double>(gains[static_cast<std::size_t>(c)]);
            l += g * g;
        }
        for (const int c : {1, 5, 7, 9, 11}) {
            const auto g = static_cast<double>(gains[static_cast<std::size_t>(c)]);
            r += g * g;
        }
        const double contrast_db = 10.0 * std::log10(l / r);
        MESSAGE("order " << order << ": left speakers " << contrast_db << " dB above right for a plane wave from the left");
        CHECK(contrast_db > (order == 1 ? 9.0 : order == 2 ? 14.0 : 16.0));  // 10.8 / 16.1 / 18.5 measured
    }
}
