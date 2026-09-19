// Steam Audio 4.8.1 with Embree: a scene whose contents are changed twice (instances or static
// meshes, removed and added, in any order, with or without re-setting the simulator's scene)
// stops reporting hits to the simulator. Found in game: occlusion vanished once a door opened.
// Steam Audio's own ray tracer is unaffected. The world scene's workaround, a fresh top-level
// scene per change with the simulator switched to it, is checked here on both ray tracers; the
// in-place edits are only reported (so a fixed Steam Audio does not fail the build).

#include "steam/ipl_handle.hpp"
#include "steam/steam_context.hpp"

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace vsa;

struct Sub {
    steam::Scene scene;
    steam::StaticMesh mesh;
};

// A 1 m thick wall at x0..x0+1 spanning y,z 0..32, as a static mesh in `scene`.
steam::StaticMesh wall_mesh(IPLScene scene, float x0) {
    steam::StaticMesh mesh;
    std::array<IPLVector3, 8> v{{{x0, 0, 0}, {x0, 32, 0}, {x0, 32, 32}, {x0, 0, 32},
                                 {x0 + 1, 0, 0}, {x0 + 1, 32, 0}, {x0 + 1, 32, 32}, {x0 + 1, 0, 32}}};
    std::array<IPLTriangle, 4> t{{{{0, 1, 2}}, {{0, 2, 3}}, {{4, 6, 5}}, {{4, 7, 6}}}};
    std::array<IPLint32, 4> m{};
    IPLMaterial material{{0.1f, 0.1f, 0.1f}, 0.05f, {0.1f, 0.1f, 0.1f}};
    IPLStaticMeshSettings ms{};
    ms.numVertices = 8;
    ms.numTriangles = 4;
    ms.numMaterials = 1;
    ms.vertices = v.data();
    ms.triangles = t.data();
    ms.materialIndices = m.data();
    ms.materials = &material;
    REQUIRE(iplStaticMeshCreate(scene, &ms, mesh.out()) == IPL_STATUS_SUCCESS);
    iplStaticMeshAdd(mesh.get(), scene);
    iplSceneCommit(scene);
    return mesh;
}

Sub make_wall(const steam::SteamContext& steam, float x0) {
    Sub s;
    IPLSceneSettings settings = steam.scene_settings();
    REQUIRE(iplSceneCreate(steam.context(), &settings, s.scene.out()) == IPL_STATUS_SUCCESS);
    s.mesh = wall_mesh(s.scene.get(), x0);
    return s;
}

IPLMatrix4x4 identity() {
    IPLMatrix4x4 m{};
    for (int i = 0; i < 4; ++i) {
        m.elements[i][i] = 1.0f;
    }
    return m;
}

struct Probe {
    steam::Simulator sim;
    steam::Source source;
    Probe(const steam::SteamContext& steam, IPLScene scene) {
        IPLSimulationSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        settings.sceneType = steam.scene_type();
        settings.maxNumOcclusionSamples = 1;
        settings.samplingRate = 48000;
        settings.frameSize = 256;
        REQUIRE(iplSimulatorCreate(steam.context(), &settings, sim.out()) == IPL_STATUS_SUCCESS);
        iplSimulatorSetScene(sim.get(), scene);
        iplSimulatorCommit(sim.get());
        IPLSourceSettings ss{};
        ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
        REQUIRE(iplSourceCreate(sim.get(), &ss, source.out()) == IPL_STATUS_SUCCESS);
        iplSourceAdd(source.get(), sim.get());
        iplSimulatorCommit(sim.get());
    }
    ~Probe() {
        iplSourceRemove(source.get(), sim.get());
        iplSimulatorCommit(sim.get());
    }
    float occlusion() {
        iplSimulatorCommit(sim.get());
        IPLSimulationSharedInputs shared{};
        shared.listener.right = {1, 0, 0};
        shared.listener.up = {0, 1, 0};
        shared.listener.ahead = {0, 0, -1};
        shared.listener.origin = {4, 16, 16};
        iplSimulatorSetSharedInputs(sim.get(), IPL_SIMULATIONFLAGS_DIRECT, &shared);
        IPLSimulationInputs in{};
        in.flags = IPL_SIMULATIONFLAGS_DIRECT;
        in.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
        in.source.right = {1, 0, 0};
        in.source.up = {0, 1, 0};
        in.source.ahead = {0, 0, -1};
        in.source.origin = {20, 16, 16};
        in.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
        iplSourceSetInputs(source.get(), IPL_SIMULATIONFLAGS_DIRECT, &in);
        iplSimulatorRunDirect(sim.get());
        IPLSimulationOutputs out{};
        iplSourceGetOutputs(source.get(), IPL_SIMULATIONFLAGS_DIRECT, &out);
        return out.direct.occlusion;
    }
};

}  // namespace

TEST_CASE("Steam Audio scene edits: in place (reported), and a fresh top-level scene per change (the workaround)") {
    for (const vsa_ray_tracer tracer : {VSA_RAY_TRACER_STEAM, VSA_RAY_TRACER_AUTO}) {
        steam::SteamContext steam({tracer, false});
        IPLSceneSettings settings = steam.scene_settings();

        // A: replace the instance (remove old, add new, commit, release old afterwards).
        // B: remove old, commit, release, then add new, commit.
        // C: keep the instance, swap the static mesh inside its sub-scene.
        for (const char variant : {'A', 'B', 'C', 'D'}) {
            steam::Scene top;
            REQUIRE(iplSceneCreate(steam.context(), &settings, top.out()) == IPL_STATUS_SUCCESS);
            Sub sub = make_wall(steam, 10);
            steam::InstancedMesh instance;
            IPLInstancedMeshSettings is{sub.scene.get(), identity()};
            REQUIRE(iplInstancedMeshCreate(top.get(), &is, instance.out()) == IPL_STATUS_SUCCESS);
            iplInstancedMeshAdd(instance.get(), top.get());
            iplSceneCommit(top.get());
            Probe probe(steam, top.get());
            std::vector<float> seen{probe.occlusion()};
            for (int round = 0; round < 3; ++round) {
                if (variant == 'C') {
                    iplStaticMeshRemove(sub.mesh.get(), sub.scene.get());
                    iplSceneCommit(sub.scene.get());
                    sub.mesh = wall_mesh(sub.scene.get(), 10);
                    iplSceneCommit(top.get());
                    seen.push_back(probe.occlusion());
                    continue;
                }
                Sub next = make_wall(steam, 10);
                steam::InstancedMesh next_instance;
                IPLInstancedMeshSettings ns{next.scene.get(), identity()};
                if (variant == 'A' || variant == 'D') {
                    iplInstancedMeshRemove(instance.get(), top.get());
                    REQUIRE(iplInstancedMeshCreate(top.get(), &ns, next_instance.out()) == IPL_STATUS_SUCCESS);
                    iplInstancedMeshAdd(next_instance.get(), top.get());
                    iplSceneCommit(top.get());
                    instance = std::move(next_instance);
                    sub = std::move(next);
                    if (variant == 'D') {
                        iplSimulatorSetScene(probe.sim.get(), top.get());
                    }
                } else {
                    iplInstancedMeshRemove(instance.get(), top.get());
                    iplSceneCommit(top.get());
                    instance.reset();
                    sub = std::move(next);
                    REQUIRE(iplInstancedMeshCreate(top.get(), &ns, next_instance.out()) == IPL_STATUS_SUCCESS);
                    iplInstancedMeshAdd(next_instance.get(), top.get());
                    iplSceneCommit(top.get());
                    instance = std::move(next_instance);
                }
                seen.push_back(probe.occlusion());
            }
            MESSAGE("tracer " << tracer << " variant " << variant << ": " << seen[0] << " " << seen[1] << " " << seen[2]
                              << " " << seen[3]);
            iplInstancedMeshRemove(instance.get(), top.get());
            iplSceneCommit(top.get());
        }
    }

    // E: no instancing, static meshes straight in the top scene, replaced repeatedly (and other
    // meshes coming and going alongside).
    for (const vsa_ray_tracer tracer : {VSA_RAY_TRACER_STEAM, VSA_RAY_TRACER_AUTO}) {
        steam::SteamContext steam({tracer, false});
        IPLSceneSettings settings = steam.scene_settings();
        steam::Scene top;
        REQUIRE(iplSceneCreate(steam.context(), &settings, top.out()) == IPL_STATUS_SUCCESS);
        steam::StaticMesh mesh = wall_mesh(top.get(), 10);
        steam::StaticMesh other = wall_mesh(top.get(), 40);
        iplSceneCommit(top.get());
        Probe probe(steam, top.get());
        std::vector<float> seen{probe.occlusion()};
        for (int round = 0; round < 6; ++round) {
            iplStaticMeshRemove(mesh.get(), top.get());
            steam::StaticMesh next = wall_mesh(top.get(), round % 2 == 0 ? 10.0f : 10.5f);
            iplSceneCommit(top.get());
            mesh = std::move(next);
            if (round == 2) {
                iplStaticMeshRemove(other.get(), top.get());
                iplSceneCommit(top.get());
                other = wall_mesh(top.get(), 50);
            }
            seen.push_back(probe.occlusion());
        }
        std::string line;
        for (float f : seen) {
            line += std::to_string(f).substr(0, 4) + " ";
        }
        MESSAGE("tracer " << tracer << " variant E: " << line);
        iplStaticMeshRemove(mesh.get(), top.get());
        iplStaticMeshRemove(other.get(), top.get());
        iplSceneCommit(top.get());
    }

    // G: never edit a top-level scene; each change builds a new one with instances of the
    // (immutable) chunk sub-scenes, and the simulator switches to it.
    for (const vsa_ray_tracer tracer : {VSA_RAY_TRACER_STEAM, VSA_RAY_TRACER_AUTO}) {
        steam::SteamContext steam({tracer, false});
        IPLSceneSettings settings = steam.scene_settings();
        Sub wall = make_wall(steam, 10);
        Sub other = make_wall(steam, 40);
        struct Top {
            steam::Scene scene;
            std::vector<steam::InstancedMesh> instances;
        };
        const auto build = [&](std::vector<IPLScene> subs) {
            auto top = std::make_unique<Top>();
            REQUIRE(iplSceneCreate(steam.context(), &settings, top->scene.out()) == IPL_STATUS_SUCCESS);
            for (IPLScene sub : subs) {
                IPLInstancedMeshSettings is{sub, identity()};
                steam::InstancedMesh m;
                REQUIRE(iplInstancedMeshCreate(top->scene.get(), &is, m.out()) == IPL_STATUS_SUCCESS);
                iplInstancedMeshAdd(m.get(), top->scene.get());
                top->instances.push_back(std::move(m));
            }
            iplSceneCommit(top->scene.get());
            return top;
        };
        const auto retire = [](std::unique_ptr<Top>& top) {
            for (auto& m : top->instances) {
                iplInstancedMeshRemove(m.get(), top->scene.get());
            }
            iplSceneCommit(top->scene.get());
            top.reset();
        };
        std::unique_ptr<Top> top = build({wall.scene.get(), other.scene.get()});
        Probe probe(steam, top->scene.get());
        std::vector<float> seen{probe.occlusion()};
        for (int round = 0; round < 6; ++round) {
            Sub next = make_wall(steam, round % 2 == 0 ? 10.0f : 10.5f);
            std::unique_ptr<Top> fresh = build({next.scene.get(), other.scene.get()});
            iplSimulatorSetScene(probe.sim.get(), fresh->scene.get());
            iplSimulatorCommit(probe.sim.get());
            retire(top);
            top = std::move(fresh);
            wall = std::move(next);
            seen.push_back(probe.occlusion());
        }
        std::string line;
        for (float f : seen) {
            line += std::to_string(f).substr(0, 4) + " ";
        }
        MESSAGE("tracer " << tracer << " fresh top-level scene per change: " << line);
        for (const float occlusion : seen) {
            CHECK(static_cast<double>(occlusion) < 0.01);
        }
        iplSimulatorSetScene(probe.sim.get(), nullptr);
        retire(top);
    }
}
