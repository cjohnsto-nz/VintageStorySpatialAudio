// Our order-3 spherical harmonics against Steam Audio's own Ambisonics encoder: the engine
// encodes the world bus itself (interpolated, straight into the bus) and relies on Steam Audio's
// decoder, so the conventions must match exactly.

#include "dsp/spherical_harmonics.hpp"
#include "steam/ipl_handle.hpp"
#include "steam/steam_context.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

TEST_CASE("order-3 spherical harmonics match Steam Audio's Ambisonics encoder") {
    constexpr int kFrames = 64;
    constexpr int kChannels = static_cast<int>(vsa::dsp::kSh3Channels);
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_STEAM, false});
    IPLAudioSettings audio{48000, kFrames};
    IPLAmbisonicsEncodeEffectSettings settings{3};
    vsa::steam::AmbisonicsEncodeEffect encoder;
    REQUIRE(iplAmbisonicsEncodeEffectCreate(steam.context(), &audio, &settings, encoder.out()) == IPL_STATUS_SUCCESS);

    std::vector<float> ones(kFrames, 1.0f);
    std::vector<float> storage(static_cast<std::size_t>(kFrames) * kChannels);
    float* in_channels[1] = {ones.data()};
    float* out_channels[kChannels];
    for (int c = 0; c < kChannels; ++c) {
        out_channels[c] = storage.data() + static_cast<std::ptrdiff_t>(c) * kFrames;
    }
    IPLAudioBuffer in{1, kFrames, in_channels};
    IPLAudioBuffer out{kChannels, kFrames, out_channels};

    std::mt19937 rng(7);
    std::normal_distribution<float> normal;
    std::vector<IPLVector3> directions = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int i = 0; i < 64; ++i) {
        const float x = normal(rng);
        const float y = normal(rng);
        const float z = normal(rng);
        const float n = std::sqrt(x * x + y * y + z * z);
        directions.push_back({x / n, y / n, z / n});
    }

    for (const IPLVector3& d : directions) {
        CAPTURE(d.x);
        CAPTURE(d.y);
        CAPTURE(d.z);
        IPLAmbisonicsEncodeEffectParams params{};
        params.direction = d;
        params.order = 3;
        // Steam Audio's first block after a direction change is a transition; compare the steady state.
        iplAmbisonicsEncodeEffectApply(encoder.get(), &params, &in, &out);
        iplAmbisonicsEncodeEffectApply(encoder.get(), &params, &in, &out);

        const float direction[3] = {d.x, d.y, d.z};
        float ours[vsa::dsp::kSh3Channels];
        vsa::dsp::sh_order3(direction, ours);
        for (int c = 0; c < kChannels; ++c) {
            CAPTURE(c);
            CHECK(static_cast<double>(ours[c]) ==
                  doctest::Approx(static_cast<double>(out_channels[c][kFrames - 1])).epsilon(1e-4).scale(1.0));
        }
    }
}
