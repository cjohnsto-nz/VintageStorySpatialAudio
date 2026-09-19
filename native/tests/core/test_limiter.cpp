#include "dsp/limiter.hpp"
#include "support/signals.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

using vsa::dsp::Limiter;

namespace {

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

// Runs the limiter over the whole signal in 256-frame blocks; returns the smallest gain applied.
float run(Limiter& limiter, Stereo& signal) {
    float smallest = 1.0f;
    for (std::size_t off = 0; off < signal.left.size(); off += 256) {
        const auto n = static_cast<uint32_t>(std::min<std::size_t>(256, signal.left.size() - off));
        smallest = std::min(smallest, limiter.process(signal.left.data() + off, signal.right.data() + off, n));
    }
    return smallest;
}

Stereo stereo_sine(double frequency, float amplitude, std::size_t frames, double phase = 0.0) {
    Stereo s;
    s.left.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        s.left[i] = static_cast<float>(static_cast<double>(amplitude) *
                                       std::sin(2.0 * std::numbers::pi * frequency / 48000.0 * static_cast<double>(i) + phase));
    }
    s.right = s.left;
    return s;
}

// Peak of a band-limited signal between samples, by 16x sinc interpolation (reference, slow).
double true_peak(const std::vector<float>& x) {
    double peak = 0.0;
    const auto n = static_cast<int>(x.size());
    for (int i = 16; i < n - 16; ++i) {
        for (int p = 0; p < 16; ++p) {
            const double t = i + p / 16.0;
            double sum = 0.0;
            for (int k = i - 15; k <= i + 16; ++k) {
                const double d = t - k;
                const double sinc = std::abs(d) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
                const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * d / 16.0);
                sum += static_cast<double>(x[static_cast<std::size_t>(k)]) * sinc * w;
            }
            peak = std::max(peak, std::abs(sum));
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("the limiter passes quiet material unchanged, delayed by its latency") {
    Limiter limiter;
    limiter.prepare(48000);
    CHECK(limiter.latency_frames() == 4 + 96 - 1);

    Stereo signal = stereo_sine(440.0, 0.5f, 4800);
    const Stereo original = signal;
    CHECK(run(limiter, signal) == 1.0f);
    const std::size_t latency = limiter.latency_frames();
    for (std::size_t i = latency; i < signal.left.size(); ++i) {
        REQUIRE(signal.left[i] == original.left[i - latency]);
    }
}

TEST_CASE("a +12 dB sine is held under the -1 dBTP ceiling") {
    Limiter limiter;
    limiter.prepare(48000);
    Stereo signal = stereo_sine(1000.0, 4.0f, 48000);
    const float smallest = run(limiter, signal);
    CHECK(smallest < 0.25f);
    CHECK(vsa_test::peak(signal.left) <= static_cast<double>(limiter.ceiling()) + 1e-5);
    // Inter-sample peaks too (within the 4x detector's accuracy).
    const std::vector<float> settled(signal.left.begin() + 4800, signal.left.end());
    CHECK(vsa_test::to_db(true_peak(settled)) <= -1.0 + 0.3);
}

TEST_CASE("inter-sample peaks are detected (fs/4 at 45 degrees)") {
    // Samples sit at +-0.707 A while the waveform peaks at A: 1.2 A would pass a sample-peak limiter.
    Limiter limiter;
    limiter.prepare(48000);
    Stereo signal = stereo_sine(12000.0, 1.2f, 9600, std::numbers::pi / 4.0);
    CHECK(vsa_test::peak(signal.left) < static_cast<double>(limiter.ceiling()));  // sample peaks are below the ceiling
    CHECK(run(limiter, signal) < 0.8f);
    const std::vector<float> settled(signal.left.begin() + 2400, signal.left.end());
    CHECK(vsa_test::to_db(true_peak(settled)) <= -1.0 + 0.3);
}

TEST_CASE("an isolated spike is caught before it passes and the gain recovers afterwards") {
    Limiter limiter;
    limiter.prepare(48000);
    Stereo signal = stereo_sine(200.0, 0.3f, 48000);
    signal.left[1000] = 2.0f;
    signal.right[1000] = -2.0f;
    run(limiter, signal);
    CHECK(vsa_test::peak(signal.left) <= static_cast<double>(limiter.ceiling()) + 1e-5);
    CHECK(vsa_test::peak(signal.right) <= static_cast<double>(limiter.ceiling()) + 1e-5);
    // Half a second later the 0.3 sine is back at full level.
    const std::vector<float> tail(signal.left.begin() + 30000, signal.left.end());
    CHECK(vsa_test::peak(tail) == doctest::Approx(0.3).epsilon(0.01));
}
