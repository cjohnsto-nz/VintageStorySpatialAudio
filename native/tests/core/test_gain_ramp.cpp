#include "dsp/gain_ramp.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using vsa::dsp::GainRamp;

TEST_CASE("a linear ramp lands exactly on its target and holds it") {
    GainRamp ramp(0.0f);
    ramp.linear(1.0f, 100);
    std::vector<float> out(256);
    CHECK(ramp.render(out.data(), 256));
    CHECK(out[49] == doctest::Approx(0.5).epsilon(1e-5));
    CHECK(out[99] == 1.0f);
    CHECK(out[255] == 1.0f);
    CHECK_FALSE(ramp.active());
    CHECK_FALSE(ramp.render(out.data(), 16));  // idle: no completion reported again
}

TEST_CASE("a geometric fade is linear in decibels") {
    GainRamp ramp(1.0f);
    ramp.geometric(0.01f, 1000);  // 0 dB -> -40 dB
    std::vector<float> out(1000);
    CHECK(ramp.render(out.data(), 1000));
    for (const int i : {99, 249, 499, 749}) {
        const double expected_db = -40.0 * (i + 1) / 1000.0;
        CHECK(20.0 * std::log10(static_cast<double>(out[static_cast<std::size_t>(i)])) ==
              doctest::Approx(expected_db).epsilon(1e-4));
    }
    CHECK(out[999] == 0.01f);
}

TEST_CASE("fades to and from silence use a -80 dB floor and end exactly on zero") {
    GainRamp down(1.0f);
    down.geometric(0.0f, 480);
    std::vector<float> out(480);
    CHECK(down.render(out.data(), 480));
    CHECK(out[479] == 0.0f);
    CHECK(20.0 * std::log10(static_cast<double>(out[239])) == doctest::Approx(-40.0).epsilon(1e-3));

    GainRamp up(0.0f);
    up.geometric(1.0f, 480);
    CHECK(up.render(out.data(), 480));
    CHECK(out[0] > 0.0f);
    CHECK(out[479] == 1.0f);
}

TEST_CASE("advancing without output reaches the same value as rendering") {
    GainRamp a(1.0f);
    GainRamp b(1.0f);
    a.geometric(0.25f, 1000);
    b.geometric(0.25f, 1000);
    std::vector<float> out(300);
    a.render(out.data(), 300);
    b.render(nullptr, 300);
    CHECK(a.value() == doctest::Approx(static_cast<double>(b.value())).epsilon(1e-6));
    CHECK(a.render(nullptr, 700));
    CHECK(b.render(nullptr, 700));
    CHECK(a.value() == 0.25f);
    CHECK(b.value() == 0.25f);
}
