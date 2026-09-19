// The late reverb continues the simulated impulse response (ADR 0009). The reference is Steam
// Audio's own simulation rendered in full by convolution; the tail our renderer puts after the
// convolved early part (from the hybrid simulation's decay times, level and delay) must carry
// on at the reference's level. (Steam Audio's own hybrid tail sits some 6 dB below it.)

#include "dsp/late_reverb.hpp"
#include "steam/ipl_handle.hpp"
#include "steam/steam_context.hpp"
#include "world/reflection_channel.hpp"
#include "world/reflection_sim.hpp"
#include "world/world_scene.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace vsa::world;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 256;
constexpr int kChannels = 9;  // order 2
constexpr uint32_t kRays = 4096;
constexpr uint32_t kBounces = 64;  // enough that the reference is not cut short in the window
constexpr float kDuration = 2.0f;
constexpr float kTransition = 0.1f;
constexpr float kY00 = 0.28209479f;

struct Room {
    int w, h, d;
    float absorption;
};

std::shared_ptr<ChunkVoxels> room_chunk(const Room& r) {
    auto c = std::make_shared<ChunkVoxels>();
    for (int y = 1; y <= 2 + r.h; ++y) {
        for (int z = 1; z <= 2 + r.d; ++z) {
            for (int x = 1; x <= 2 + r.w; ++x) {
                const bool inside = x >= 2 && x < 2 + r.w && y >= 2 && y < 2 + r.h && z >= 2 && z < 2 + r.d;
                if (!inside) {
                    c->materials[static_cast<std::size_t>(cell_index(x, y, z))] = 1;
                }
            }
        }
    }
    return c;
}

/// Renders an impulse through a reflection effect of `type`, returning the W channel.
std::vector<float> render_w(const vsa::steam::SteamContext& steam, IPLReflectionEffectType type, int ir_size,
                            const IPLReflectionEffectParams& params, std::size_t frames) {
    IPLAudioSettings audio{static_cast<IPLint32>(kRate), static_cast<IPLint32>(kBlock)};
    IPLReflectionEffectSettings settings{type, ir_size, kChannels};
    vsa::steam::ReflectionEffect effect;
    REQUIRE(iplReflectionEffectCreate(steam.context(), &audio, &settings, effect.out()) == IPL_STATUS_SUCCESS);
    std::vector<float> w(frames, 0.0f);
    std::vector<float> in(kBlock), storage(static_cast<std::size_t>(kChannels) * kBlock);
    std::array<float*, kChannels> out{};
    for (int k = 0; k < kChannels; ++k) {
        out[static_cast<std::size_t>(k)] = storage.data() + static_cast<std::size_t>(k) * kBlock;
    }
    for (std::size_t at = 0; at + kBlock <= frames; at += kBlock) {
        std::fill(in.begin(), in.end(), 0.0f);
        if (at == 0) {
            in[0] = 1.0f;
        }
        float* in_channels[1] = {in.data()};
        IPLAudioBuffer in_buffer{1, static_cast<IPLint32>(kBlock), in_channels};
        IPLAudioBuffer out_buffer{kChannels, static_cast<IPLint32>(kBlock), out.data()};
        IPLReflectionEffectParams p = params;
        p.type = type;
        p.numChannels = kChannels;
        iplReflectionEffectApply(effect.get(), &p, &in_buffer, &out_buffer, nullptr);
        std::copy_n(out[0], kBlock, w.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return w;
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) {
        e += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return e;
}

/// Steam Audio's convolution simulation of the room, rendered in full (the reference).
std::vector<float> reference(const vsa::steam::SteamContext& steam, WorldScene& scene, const float at[3],
                             std::size_t frames) {
    IPLSimulationSettings ss{};
    ss.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    ss.sceneType = steam.scene_type();
    ss.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    ss.maxNumRays = kRays;
    ss.numDiffuseSamples = 32;
    ss.maxDuration = kDuration;
    ss.maxOrder = 2;
    ss.maxNumSources = 1;
    ss.numThreads = 2;
    ss.samplingRate = kRate;
    ss.frameSize = kBlock;
    vsa::steam::Simulator simulator;
    REQUIRE(iplSimulatorCreate(steam.context(), &ss, simulator.out()) == IPL_STATUS_SUCCESS);
    scene.attach(simulator.get());
    vsa::steam::Source source;
    IPLSourceSettings source_settings{IPL_SIMULATIONFLAGS_REFLECTIONS};
    REQUIRE(iplSourceCreate(simulator.get(), &source_settings, source.out()) == IPL_STATUS_SUCCESS);
    iplSourceAdd(source.get(), simulator.get());
    iplSimulatorCommit(simulator.get());
    IPLSimulationInputs inputs{};
    inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    inputs.source.right = {1.0f, 0.0f, 0.0f};
    inputs.source.up = {0.0f, 1.0f, 0.0f};
    inputs.source.ahead = {0.0f, 0.0f, -1.0f};
    inputs.source.origin = {at[0], at[1], at[2]};
    inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1.0f;
    iplSourceSetInputs(source.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
    IPLSimulationSharedInputs shared{};
    shared.listener = inputs.source;
    shared.numRays = kRays;
    shared.numBounces = kBounces;
    shared.duration = kDuration;
    shared.order = 2;
    shared.irradianceMinDistance = 1.0f;
    iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
    for (int i = 0; i < 10; ++i) {
        iplSimulatorRunReflections(simulator.get());
    }
    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(source.get(), IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
    std::vector<float> w = render_w(steam, IPL_REFLECTIONEFFECTTYPE_CONVOLUTION,
                                    static_cast<int>(kDuration * kRate), outputs.reflections, frames);
    iplSourceRemove(source.get(), simulator.get());
    iplSimulatorCommit(simulator.get());
    scene.detach(simulator.get());
    return w;
}

}  // namespace

TEST_CASE("the late reverb continues the simulated impulse response (small room, large room, cave)") {
    const vsa::steam::SteamContext steam({VSA_RAY_TRACER_AUTO, false});
    // The first three fitted the calibration; the last two check it.
    const Room rooms[] = {{4, 3, 4, 0.2f}, {12, 6, 12, 0.2f}, {26, 14, 26, 0.2f}, {8, 4, 8, 0.3f}, {16, 8, 16, 0.1f}};
    for (const Room& room : rooms) {
        CAPTURE(room.w);
        WorldScene scene(steam);
        AcousticMaterial air;
        air.kind = MaterialKind::Air;
        AcousticMaterial stone;
        stone.kind = MaterialKind::Solid;
        stone.absorption[0] = stone.absorption[1] = stone.absorption[2] = room.absorption;
        stone.scattering = 0.3f;
        scene.set_materials({air, stone});
        scene.set_chunk({0, 0, 0}, room_chunk(room), 0);
        REQUIRE(scene.wait_idle(std::chrono::seconds(10)));
        const float at[3] = {2.0f + static_cast<float>(room.w) / 2.0f, 2.0f + std::min(1.7f, static_cast<float>(room.h) / 2.0f),
                             2.0f + static_cast<float>(room.d) / 2.0f};
        const std::size_t frames = static_cast<std::size_t>(kRate) * 2;

        // Our renderer's view: the hybrid simulation's parameters, and the late reverb from them.
        ReflectionChannel channel(1);
        ReflectionSettings settings;
        settings.rays = kRays;
        settings.bounces = kBounces;
        settings.duration = kDuration;
        settings.transition = kTransition;
        settings.sources = 0;
        ReflectionSimulator sim(steam, scene, channel, settings, kRate, kBlock);
        vsa::ListenerPose pose;
        std::copy_n(at, 3, pose.position);
        sim.set_listener(pose);
        for (int i = 0; i < 10; ++i) {
            sim.tick();
        }
        float rt[3];
        float eq[3];
        for (int b = 0; b < 3; ++b) {
            rt[b] = channel.output(0).reverb_times[b].load();
            eq[b] = channel.output(0).eq[b].load();
        }
        const int32_t delay = channel.output(0).delay.load();
        vsa::dsp::LateReverb late;
        late.prepare(kRate, 9600);
        std::vector<float> ours(frames, 0.0f);
        {
            std::vector<float> in(kBlock);
            std::vector<float> storage(static_cast<std::size_t>(vsa::dsp::LateReverb::kOutputs) * kBlock);
            std::array<float*, vsa::dsp::LateReverb::kOutputs> out{};
            for (int k = 0; k < vsa::dsp::LateReverb::kOutputs; ++k) {
                out[static_cast<std::size_t>(k)] = storage.data() + static_cast<std::size_t>(k) * kBlock;
            }
            for (std::size_t pos = 0; pos + kBlock <= frames; pos += kBlock) {
                std::fill(in.begin(), in.end(), 0.0f);
                if (pos == 0) {
                    in[0] = 1.0f;
                }
                std::fill(storage.begin(), storage.end(), 0.0f);
                late.process(in.data(), kBlock, rt, eq, static_cast<uint32_t>(delay), out.data());
                for (uint32_t j = 0; j < kBlock; ++j) {
                    float sum = 0.0f;
                    for (const float* o : out) {
                        sum += o[j];
                    }
                    ours[pos + j] = sum * kY00 / std::sqrt(static_cast<float>(vsa::dsp::LateReverb::kOutputs));
                }
            }
        }
        // Steam Audio's own hybrid tail, for the record.
        IPLReflectionEffectParams hybrid{};
        hybrid.ir = nullptr;  // the parametric part alone
        std::copy_n(rt, 3, hybrid.reverbTimes);
        std::copy_n(eq, 3, hybrid.eq);
        hybrid.delay = delay;
        hybrid.irSize = static_cast<IPLint32>(kTransition * kRate);
        const std::vector<float> steam_tail =
            render_w(steam, IPL_REFLECTIONEFFECTTYPE_HYBRID, static_cast<int>(kTransition * kRate), hybrid, frames);

        const std::vector<float> truth = reference(steam, scene, at, frames);
        // Just after the transition, over a quarter of the decay time (15 dB of decay).
        const auto from = static_cast<std::size_t>((kTransition + 0.03f) * kRate);
        const auto to = from + static_cast<std::size_t>(std::min(0.3f, rt[1] / 4.0f) * kRate);
        const double ours_db = 10.0 * std::log10(energy(ours, from, to) / energy(truth, from, to));
        const double steam_db = 10.0 * std::log10(energy(steam_tail, from, to) / energy(truth, from, to));
        MESSAGE("room " << room.w << "x" << room.h << "x" << room.d << " (RT60 " << rt[1] << " s): our tail " << ours_db
                        << " dB from the simulated response, Steam Audio's hybrid tail " << steam_db << " dB");
        // 0.7 dB in the fitted rooms. In the 16 x 8 x 16 check room 2.5-3.2 dB under: its reference
        // itself varies between simulator instances (Steam Audio's own tail moves with it).
        CHECK(std::abs(ours_db) < 3.5);
    }
}
