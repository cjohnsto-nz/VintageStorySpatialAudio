// The diffuse late reverb (ADR 0009): it decays at the requested RT60 per band, its outputs are
// decorrelated, its level follows the requested one, it goes quiet, and it never allocates. (Its
// level against the simulation: core/test_reflection_calibration.cpp.)

#include "core/alloc_counter.hpp"
#include "dsp/late_reverb.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using vsa::dsp::LateReverb;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 256;

struct Output {
    std::array<std::vector<float>, LateReverb::kOutputs> channels;
    /// Pressure: the outputs' sum over sqrt(n) (as the renderer encodes them).
    [[nodiscard]] std::vector<float> pressure() const {
        std::vector<float> p(channels[0].size(), 0.0f);
        const float scale = 1.0f / std::sqrt(static_cast<float>(LateReverb::kOutputs));
        for (const auto& c : channels) {
            for (std::size_t i = 0; i < p.size(); ++i) {
                p[i] += c[i] * scale;
            }
        }
        return p;
    }
};

/// Feeds `input` (padded with silence to `frames`) through a reverb block by block.
Output run(LateReverb& reverb, const std::vector<float>& input, std::size_t frames, const float rt[3],
           const float level[3], uint32_t predelay) {
    Output out;
    for (auto& c : out.channels) {
        c.assign(frames, 0.0f);
    }
    std::vector<float> in(kBlock);
    std::array<float*, LateReverb::kOutputs> ptr{};
    for (std::size_t at = 0; at + kBlock <= frames; at += kBlock) {
        for (uint32_t j = 0; j < kBlock; ++j) {
            in[j] = at + j < input.size() ? input[at + j] : 0.0f;
        }
        for (int k = 0; k < LateReverb::kOutputs; ++k) {
            ptr[static_cast<std::size_t>(k)] = out.channels[static_cast<std::size_t>(k)].data() + at;
        }
        reverb.process(in.data(), kBlock, rt, level, predelay, ptr.data());
    }
    return out;
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
        e += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return e;
}

/// RT60 from the energy decay between two windows `gap` seconds apart.
double decay_rt60(const std::vector<float>& x, double start, double gap, double window) {
    const auto w = static_cast<std::size_t>(window * kRate);
    const auto a = static_cast<std::size_t>(start * kRate);
    const auto b = static_cast<std::size_t>((start + gap) * kRate);
    const double drop_db = 10.0 * std::log10(energy(x, a, a + w) / energy(x, b, b + w));
    return 60.0 * gap / drop_db;
}

/// A second-order band-pass (RBJ, constant 0 dB peak) applied in place.
void bandpass(std::vector<float>& x, double frequency, double q) {
    const double w = 2.0 * std::numbers::pi * frequency / kRate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double b0 = alpha / a0;
    const double b2 = -alpha / a0;
    const double a1 = -2.0 * std::cos(w) / a0;
    const double a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (float& s : x) {
        const double in = static_cast<double>(s);
        const double y = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = y;
        s = static_cast<float>(y);
    }
}

std::vector<float> impulse() { return {1.0f}; }

}  // namespace

TEST_CASE("late reverb: decays at the requested RT60") {
    for (const float rt : {0.5f, 1.0f, 2.0f}) {
        CAPTURE(rt);
        LateReverb reverb;
        reverb.prepare(kRate, 4800);
        const float rts[3] = {rt, rt, rt};
        const float level[3] = {1.0f, 1.0f, 1.0f};
        const Output out = run(reverb, impulse(), static_cast<std::size_t>(kRate) * 3, rts, level, 0);
        const std::vector<float> p = out.pressure();
        const double measured = decay_rt60(p, 0.1, std::min(0.8, static_cast<double>(rt)), 0.05);
        MESSAGE("RT60 " << rt << " s: measured " << measured << " s");
        CHECK(measured == doctest::Approx(static_cast<double>(rt)).epsilon(0.15));
    }
}

TEST_CASE("late reverb: each band decays at its own RT60") {
    LateReverb reverb;
    reverb.prepare(kRate, 4800);
    const float rts[3] = {2.0f, 1.0f, 0.4f};
    const float level[3] = {1.0f, 1.0f, 1.0f};
    const Output out = run(reverb, impulse(), static_cast<std::size_t>(kRate) * 3, rts, level, 0);
    const struct {
        double frequency;
        double expected;
        double tolerance;
    } bands[] = {{150.0, 2.0, 0.2}, {2500.0, 1.0, 0.2}, {16000.0, 0.4, 0.3}};
    for (const auto& band : bands) {
        CAPTURE(band.frequency);
        std::vector<float> p = out.pressure();
        bandpass(p, band.frequency, 2.0);
        const double measured = decay_rt60(p, 0.1, std::min(0.5, band.expected / 2.0), 0.08);
        MESSAGE(band.frequency << " Hz: RT60 " << measured << " s (asked " << band.expected << ")");
        CHECK(measured == doctest::Approx(band.expected).epsilon(band.tolerance));
    }
}

TEST_CASE("late reverb: the outputs are decorrelated, and the pre-delay holds the tail back") {
    LateReverb reverb;
    reverb.prepare(kRate, 9600);
    const float rts[3] = {1.0f, 1.0f, 1.0f};
    const float level[3] = {1.0f, 1.0f, 1.0f};
    constexpr uint32_t kPredelay = 4800;
    const Output out = run(reverb, impulse(), kRate, rts, level, kPredelay);
    // Nothing before the pre-delay.
    const std::size_t first = kPredelay;
    CHECK(energy(out.channels[0], 0, first - 1) == 0.0);
    CHECK(energy(out.pressure(), first, first + 4800) > 0.0);
    double worst = 0.0;
    for (int a = 0; a < LateReverb::kOutputs; ++a) {
        for (int b = a + 1; b < LateReverb::kOutputs; ++b) {
            const auto& x = out.channels[static_cast<std::size_t>(a)];
            const auto& y = out.channels[static_cast<std::size_t>(b)];
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (std::size_t i = first; i < x.size(); ++i) {
                xy += static_cast<double>(x[i]) * static_cast<double>(y[i]);
                xx += static_cast<double>(x[i]) * static_cast<double>(x[i]);
                yy += static_cast<double>(y[i]) * static_cast<double>(y[i]);
            }
            worst = std::max(worst, std::abs(xy) / std::sqrt(xx * yy));
        }
    }
    MESSAGE("worst output correlation " << worst);
    CHECK(worst < 0.3);
}

TEST_CASE("late reverb: the level scales the tail; silence ends it") {
    const float rts[3] = {0.3f, 0.3f, 0.3f};
    const float one[3] = {1.0f, 1.0f, 1.0f};
    const float half[3] = {0.5f, 0.5f, 0.5f};
    LateReverb a;
    a.prepare(kRate, 4800);
    LateReverb b;
    b.prepare(kRate, 4800);
    const std::vector<float> pa = run(a, impulse(), kRate, rts, one, 0).pressure();
    const std::vector<float> pb = run(b, impulse(), kRate, rts, half, 0).pressure();
    const double ratio_db = 10.0 * std::log10(energy(pa, 0, kRate) / energy(pb, 0, kRate));
    CHECK(ratio_db == doctest::Approx(6.02).epsilon(0.02));

    // After the tail has died away (-300 dB and more), process() reports silence and writes nothing.
    std::vector<float> in(kBlock, 0.0f);
    std::array<std::vector<float>, LateReverb::kOutputs> storage;
    std::array<float*, LateReverb::kOutputs> ptr{};
    for (int k = 0; k < LateReverb::kOutputs; ++k) {
        storage[static_cast<std::size_t>(k)].assign(kBlock, 0.0f);
        ptr[static_cast<std::size_t>(k)] = storage[static_cast<std::size_t>(k)].data();
    }
    bool sounding = true;
    for (int i = 0; i < 2000 && sounding; ++i) {  // ~10 s
        sounding = a.process(in.data(), kBlock, rts, one, 0, ptr.data());
    }
    CHECK_FALSE(sounding);
    for (const auto& c : storage) {
        for (const float s : c) {
            CHECK(std::isfinite(s));
        }
    }
}

TEST_CASE("late reverb: processing never allocates") {
    LateReverb reverb;
    reverb.prepare(kRate, 4800);
    const float rts[3] = {1.5f, 1.0f, 0.5f};
    const float level[3] = {0.7f, 0.5f, 0.2f};
    std::vector<float> in(kBlock, 0.1f);
    std::array<std::vector<float>, LateReverb::kOutputs> storage;
    std::array<float*, LateReverb::kOutputs> ptr{};
    for (int k = 0; k < LateReverb::kOutputs; ++k) {
        storage[static_cast<std::size_t>(k)].assign(kBlock, 0.0f);
        ptr[static_cast<std::size_t>(k)] = storage[static_cast<std::size_t>(k)].data();
    }
    vsa_test::AllocationScope scope;
    for (int i = 0; i < 200; ++i) {
        reverb.process(in.data(), kBlock, rts, level, 2400, ptr.data());
    }
    CHECK(scope.count() == 0);
}
