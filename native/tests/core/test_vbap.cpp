#include "audio/vbap.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>

using vsa::Speaker;
using vsa::Vbap;

namespace {

enum : std::size_t { FL = 0, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR };

std::array<float, 3> direction(float azimuth, float elevation) {
    const double az = static_cast<double>(azimuth) * std::numbers::pi / 180.0;
    const double el = static_cast<double>(elevation) * std::numbers::pi / 180.0;
    return {static_cast<float>(std::sin(az) * std::cos(el)), static_cast<float>(std::sin(el)),
            static_cast<float>(-std::cos(az) * std::cos(el))};
}

std::array<double, 12> gains(const Vbap& vbap, const std::array<float, 3>& d) {
    std::array<float, 12> g{};
    vbap.gains(d.data(), g.data());
    std::array<double, 12> out{};
    std::copy(g.begin(), g.end(), out.begin());
    return out;
}

double power(const std::array<double, 12>& g, std::initializer_list<std::size_t> channels) {
    double sum = 0.0;
    for (const std::size_t c : channels) {
        sum += g[c] * g[c];
    }
    return sum;
}

}  // namespace

TEST_CASE("VBAP 7.1.4: a source at a speaker plays from that speaker alone") {
    const Vbap vbap(12);
    const auto layout = vsa::steam_layout(12);
    for (std::size_t c = 0; c < 12; ++c) {
        float azimuth = 0.0f;
        float elevation = 0.0f;
        if (!Vbap::position(layout[c], azimuth, elevation)) {
            CHECK(c == LFE);
            continue;
        }
        CAPTURE(c);
        const auto g = gains(vbap, direction(azimuth, elevation));
        for (std::size_t other = 0; other < 12; ++other) {
            CHECK(g[other] == doctest::Approx(other == c ? 1.0 : 0.0).epsilon(1e-4).scale(1.0));
        }
    }
}

TEST_CASE("VBAP 7.1.4: overhead plays evenly from the heights, below from the ear-level ring") {
    const Vbap vbap(12);
    const auto up = gains(vbap, {0.0f, 1.0f, 0.0f});
    for (const std::size_t c : {TFL, TFR, TBL, TBR}) {
        CHECK(up[c] == doctest::Approx(0.5).epsilon(1e-4));
    }
    CHECK(power(up, {TFL, TFR, TBL, TBR}) == doctest::Approx(1.0).epsilon(1e-4));

    const auto down = gains(vbap, {0.0f, -1.0f, 0.0f});
    const double each = 1.0 / std::sqrt(7.0);
    for (const std::size_t c : {FL, FR, FC, BL, BR, SL, SR}) {
        CHECK(down[c] == doctest::Approx(each).epsilon(1e-4));
    }
    CHECK(power(down, {TFL, TFR, TBL, TBR}) == doctest::Approx(0.0).scale(1.0));

    // Above the front: mostly the front heights. Below the horizon: never the heights.
    const auto front_up = gains(vbap, direction(0.0f, 45.0f));
    CHECK(power(front_up, {TFL, TFR}) > 0.9);
    CHECK(front_up[TFL] == doctest::Approx(front_up[TFR]));
    const auto left_down = gains(vbap, direction(-90.0f, -30.0f));
    CHECK(power(left_down, {TFL, TFR, TBL, TBR}) == doctest::Approx(0.0).scale(1.0));
    CHECK(left_down[SL] > 0.8);
}

TEST_CASE("VBAP 7.1.4: gains are non-negative, power-normalised, continuous, and never feed the LFE") {
    const Vbap vbap(12);
    std::mt19937 rng(3);
    std::normal_distribution<float> normal;
    for (int i = 0; i < 2000; ++i) {
        std::array<float, 3> d{normal(rng), normal(rng), normal(rng)};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& v : d) {
            v /= n;
        }
        const auto g = gains(vbap, d);
        double total = 0.0;
        for (const double x : g) {
            CHECK(x >= 0.0);
            total += x * x;
        }
        CHECK(total == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(g[LFE] == 0.0);
    }

    // Behind, where BL BR TBL TBR share a plane: left/right symmetric.
    const auto back_up = gains(vbap, direction(180.0f, 20.0f));
    CHECK(back_up[BL] == doctest::Approx(back_up[BR]));
    CHECK(back_up[TBL] == doctest::Approx(back_up[TBR]));

    // Circles at several elevations in 0.5 degree steps: no gain jumps by more than a few percent.
    for (const float elevation : {-60.0f, -10.0f, 0.0f, 20.0f, 45.0f, 70.0f, 89.0f}) {
        CAPTURE(elevation);
        auto previous = gains(vbap, direction(0.0f, elevation));
        double worst = 0.0;
        for (float azimuth = 0.5f; azimuth <= 360.0f; azimuth += 0.5f) {
            const auto g = gains(vbap, direction(azimuth, elevation));
            for (std::size_t c = 0; c < 12; ++c) {
                worst = std::max(worst, std::abs(g[c] - previous[c]));
            }
            previous = g;
        }
        CHECK(worst < 0.05);
    }
}

