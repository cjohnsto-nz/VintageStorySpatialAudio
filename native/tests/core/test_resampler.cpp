// Golden tests for the bandlimited-interpolation resampler: exactness at unit rate, DC gain,
// THD+N on sines, passband flatness and alias rejection when pitching up.

#include "dsp/resampler.hpp"
#include "support/signals.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using vsa::dsp::ResamplerKernel;
using vsa::dsp::SourcePosition;

namespace {

/// Resamples a whole mono signal (silence around it) in `block`-frame calls, like the mixer does.
std::vector<float> resample(const ResamplerKernel& kernel, const std::vector<float>& source, double ratio,
                            std::size_t out_frames, uint32_t block = 256) {
    const int64_t pad = kernel.max_taps_per_side() + 4;
    std::vector<float> window(source.size() + 2 * static_cast<std::size_t>(pad), 0.0f);
    std::copy(source.begin(), source.end(), window.begin() + pad);
    const float* channels[1] = {window.data()};
    std::vector<float> scratch(kernel.scratch_floats());
    std::vector<float> out(out_frames);
    SourcePosition pos;
    for (std::size_t off = 0; off < out_frames; off += block) {
        const auto n = static_cast<uint32_t>(std::min<std::size_t>(block, out_frames - off));
        const auto span = kernel.span(pos, ratio, n);
        REQUIRE(span.first >= -pad);
        REQUIRE(span.end <= static_cast<int64_t>(source.size()) + pad);
        float* dst[1] = {out.data() + off};
        kernel.process(channels, -pad, 1, pos, ratio, dst, n, scratch.data());
    }
    return out;
}

/// Output frames that stay clear of the edges of a `source_frames` long input.
std::size_t usable_frames(const ResamplerKernel& kernel, std::size_t source_frames, double ratio) {
    const auto margin = static_cast<double>(2 * kernel.taps_per_side(ratio) + 8);
    return static_cast<std::size_t>((static_cast<double>(source_frames) - margin) / ratio);
}

/// Steady-state part of an output (skips the filter's start-up transient).
std::vector<float> steady(const std::vector<float>& x, std::size_t skip = 512) {
    return {x.begin() + static_cast<std::ptrdiff_t>(skip), x.end() - static_cast<std::ptrdiff_t>(skip)};
}

double tone_gain_db(const ResamplerKernel& kernel, double frequency, double source_rate, double output_rate,
                    double pitch = 1.0) {
    const double ratio = source_rate / output_rate * pitch;
    const auto source = vsa_test::sine(frequency, source_rate, 24000, 0.5f);
    const auto out = steady(resample(kernel, source, ratio, usable_frames(kernel, source.size(), ratio)));
    const auto fit = vsa_test::fit_sine(out.data(), out.size(), frequency * pitch, output_rate);
    return vsa_test::to_db(fit.amplitude / 0.5);
}

const char* name(vsa_resampler_quality q) {
    switch (q) {
        case VSA_RESAMPLER_LOW: return "low";
        case VSA_RESAMPLER_HIGH: return "high";
        default: return "medium";
    }
}

}  // namespace

TEST_CASE("unit ratio on whole frames is an exact copy") {
    const ResamplerKernel kernel(VSA_RESAMPLER_MEDIUM);
    const auto source = vsa_test::sine(997.0, 48000.0, 4096, 0.7f);
    const auto out = resample(kernel, source, 1.0, 4000);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == source[i]);
    }
}

TEST_CASE("DC passes at unity gain for every ratio") {
    for (const auto quality : {VSA_RESAMPLER_LOW, VSA_RESAMPLER_MEDIUM, VSA_RESAMPLER_HIGH}) {
        const ResamplerKernel kernel(quality);
        for (const double ratio : {0.37, 0.5, 44100.0 / 48000.0, 1.37, 2.0, 3.3, 8.0}) {
            const std::vector<float> source(20000, 0.5f);
            const auto out = steady(resample(kernel, source, ratio, usable_frames(kernel, source.size(), ratio)), 64);
            const auto [lo, hi] = std::minmax_element(out.begin(), out.end());
            CAPTURE(name(quality));
            CAPTURE(ratio);
            CHECK(*lo == doctest::Approx(0.5).epsilon(0.004));
            CHECK(*hi == doctest::Approx(0.5).epsilon(0.004));
        }
    }
}

TEST_CASE("THD+N of a 1 kHz tone, 44.1 kHz -> 48 kHz") {
    // Limits per quality: measured values with a few dB of margin.
    const struct {
        vsa_resampler_quality quality;
        double limit_db;
    } cases[] = {{VSA_RESAMPLER_LOW, -52.0}, {VSA_RESAMPLER_MEDIUM, -74.0}, {VSA_RESAMPLER_HIGH, -100.0}};
    for (const auto& c : cases) {
        const ResamplerKernel kernel(c.quality);
        const double ratio = 44100.0 / 48000.0;
        const auto source = vsa_test::sine(1000.0, 44100.0, 44100, 0.5f);
        const auto out = steady(resample(kernel, source, ratio, usable_frames(kernel, source.size(), ratio)));
        const auto fit = vsa_test::fit_sine(out.data(), out.size(), 1000.0, 48000.0);
        MESSAGE(std::string(name(c.quality)) << ": THD+N " << fit.thd_n_db() << " dB, gain " << vsa_test::to_db(fit.amplitude / 0.5)
                                << " dB");
        CHECK(fit.thd_n_db() < c.limit_db);
        CHECK(std::abs(vsa_test::to_db(fit.amplitude / 0.5)) < 0.1);
    }
}

TEST_CASE("passband is flat up to the rolloff region") {
    for (const auto quality : {VSA_RESAMPLER_LOW, VSA_RESAMPLER_MEDIUM, VSA_RESAMPLER_HIGH}) {
        const ResamplerKernel kernel(quality);
        std::string report;
        for (const double f : {100.0, 1000.0, 4000.0, 8000.0, 12000.0, 16000.0, 19000.0}) {
            const double g = tone_gain_db(kernel, f, 44100.0, 48000.0);
            report += std::to_string(static_cast<int>(f)) + " Hz " + std::to_string(g) + " dB; ";
            // Flat to 8 kHz on every quality; Medium to 12 kHz, High to 16 kHz.
            const double flat_to = quality == VSA_RESAMPLER_HIGH ? 16000.0 : quality == VSA_RESAMPLER_MEDIUM ? 12000.0 : 8000.0;
            const double tolerance = quality == VSA_RESAMPLER_LOW ? 0.1 : 0.02;
            if (f <= flat_to) {
                CAPTURE(f);
                CHECK(std::abs(g) < tolerance);
            }
        }
        MESSAGE(std::string(name(quality)) << ": " << report);
    }
}

TEST_CASE("pitching up rejects content that would alias") {
    for (const auto quality : {VSA_RESAMPLER_LOW, VSA_RESAMPLER_MEDIUM, VSA_RESAMPLER_HIGH}) {
        const ResamplerKernel kernel(quality);
        // Pitch 2 on a 48 kHz source: 18 kHz would land at 36 kHz, above the 24 kHz output Nyquist.
        const double ratio = 2.0;
        const auto source = vsa_test::sine(18000.0, 48000.0, 48000, 0.5f);
        const auto out = steady(resample(kernel, source, ratio, usable_frames(kernel, source.size(), ratio)));
        const double residue_db = vsa_test::to_db(vsa_test::rms(out) / (0.5 / std::sqrt(2.0)));
        // A tone well inside the new passband survives.
        const double kept_db = tone_gain_db(kernel, 4000.0, 48000.0, 48000.0, 2.0);
        MESSAGE(std::string(name(quality)) << ": alias residue " << residue_db << " dB, 4 kHz x2 gain " << kept_db << " dB");
        CHECK(residue_db < (quality == VSA_RESAMPLER_LOW ? -45.0 : quality == VSA_RESAMPLER_MEDIUM ? -70.0 : -100.0));
        CHECK(std::abs(kept_db) < 0.1);
    }
}

TEST_CASE("block size does not change the output") {
    const ResamplerKernel kernel(VSA_RESAMPLER_MEDIUM);
    const auto source = vsa_test::sine(3000.0, 44100.0, 20000, 0.5f);
    for (const double ratio : {44100.0 / 48000.0, 1.618}) {
        const std::size_t frames = usable_frames(kernel, source.size(), ratio);
        const auto a = resample(kernel, source, ratio, frames, 1);
        const auto b = resample(kernel, source, ratio, frames, 256);
        const auto c = resample(kernel, source, ratio, frames, 1000);
        CHECK(a == b);
        CHECK(b == c);
    }
}

TEST_CASE("advance() moves the position exactly like process()") {
    const ResamplerKernel kernel(VSA_RESAMPLER_MEDIUM);
    for (const double ratio : {0.3, 44100.0 / 48000.0, 1.0, 2.7}) {
        const std::vector<float> source(20000, 0.0f);
        const int64_t pad = kernel.max_taps_per_side() + 4;
        std::vector<float> window(source.size() + 2 * static_cast<std::size_t>(pad), 0.0f);
        const float* in[1] = {window.data()};
        std::vector<float> out(256);
        float* dst[1] = {out.data()};
        std::vector<float> scratch(kernel.scratch_floats());
        SourcePosition processed;
        SourcePosition advanced;
        for (int i = 0; i < 10; ++i) {
            kernel.process(in, -pad, 1, processed, ratio, dst, 256, scratch.data());
            ResamplerKernel::advance(advanced, ratio, 256);
        }
        CHECK(processed.frame == advanced.frame);
        CHECK(processed.fraction == advanced.fraction);
    }
}
