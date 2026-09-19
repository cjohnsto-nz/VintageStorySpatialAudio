#include "steam/self_test.hpp"

#include "core/error.hpp"

#include <array>
#include <chrono>
#include <string>

namespace vsa::steam {
namespace {

void check(IPLerror error, const char* what) {
    if (error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string(what) + " failed: " + error_name(error));
    }
}

// A 4 m x 4 m x 0.5 m slab centred on the origin, in the XY plane (Steam Audio uses a
// right-handed, Y-up coordinate system in metres).
struct WallMesh {
    std::array<IPLVector3, 8> vertices{{
        {-2.0f, -2.0f, -0.25f}, {2.0f, -2.0f, -0.25f}, {2.0f, 2.0f, -0.25f}, {-2.0f, 2.0f, -0.25f},
        {-2.0f, -2.0f, 0.25f},  {2.0f, -2.0f, 0.25f},  {2.0f, 2.0f, 0.25f},  {-2.0f, 2.0f, 0.25f},
    }};
    // Outward-facing (counter-clockwise) triangles for the six faces.
    std::array<IPLTriangle, 12> triangles{{
        {{0, 2, 1}}, {{0, 3, 2}},  // -Z
        {{4, 5, 6}}, {{4, 6, 7}},  // +Z
        {{0, 1, 5}}, {{0, 5, 4}},  // -Y
        {{3, 7, 6}}, {{3, 6, 2}},  // +Y
        {{0, 4, 7}}, {{0, 7, 3}},  // -X
        {{1, 2, 6}}, {{1, 6, 5}},  // +X
    }};
    std::array<IPLint32, 12> material_indices{};
    // Steam Audio's "generic" preset.
    std::array<IPLMaterial, 1> materials{{{{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}}}};
};

// Steam Audio holds references between attached objects, and releasing an object
// that is still attached leaks (e.g. a static mesh still added to an Embree scene
// leaks its Embree geometry; found with LeakSanitizer). These guards detach in
// reverse order on every exit path, including exceptions.
class MeshAttachment {
public:
    MeshAttachment(IPLStaticMesh mesh, IPLScene scene) : mesh_(mesh), scene_(scene) {
        iplStaticMeshAdd(mesh_, scene_);
        iplSceneCommit(scene_);
    }
    ~MeshAttachment() {
        iplStaticMeshRemove(mesh_, scene_);
        iplSceneCommit(scene_);
    }
    MeshAttachment(const MeshAttachment&) = delete;
    MeshAttachment& operator=(const MeshAttachment&) = delete;

private:
    IPLStaticMesh mesh_;
    IPLScene scene_;
};

class SourceAttachment {
public:
    SourceAttachment(IPLSource source, IPLSimulator simulator) : source_(source), simulator_(simulator) {
        iplSourceAdd(source_, simulator_);
        iplSimulatorCommit(simulator_);
    }
    ~SourceAttachment() {
        iplSourceRemove(source_, simulator_);
        iplSimulatorCommit(simulator_);
    }
    SourceAttachment(const SourceAttachment&) = delete;
    SourceAttachment& operator=(const SourceAttachment&) = delete;

private:
    IPLSource source_;
    IPLSimulator simulator_;
};

IPLCoordinateSpace3 pose_at(IPLVector3 origin) {
    IPLCoordinateSpace3 pose{};
    pose.right = {1.0f, 0.0f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    pose.ahead = {0.0f, 0.0f, -1.0f};
    pose.origin = origin;
    return pose;
}

float simulate_occlusion(IPLSimulator simulator, IPLSource source, IPLVector3 source_position) {
    IPLSimulationInputs inputs{};
    inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
    inputs.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
    inputs.source = pose_at(source_position);
    inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
    iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_DIRECT, &inputs);

    iplSimulatorRunDirect(simulator);

    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
    return outputs.direct.occlusion;
}

}  // namespace

vsa_self_test_report run_self_test(const SteamContext& steam) {
    const auto started = std::chrono::steady_clock::now();

    IPLSceneSettings scene_settings = steam.scene_settings();
    Scene scene;
    check(iplSceneCreate(steam.context(), &scene_settings, scene.out()), "iplSceneCreate");

    WallMesh wall;
    IPLStaticMeshSettings mesh_settings{};
    mesh_settings.numVertices = static_cast<IPLint32>(wall.vertices.size());
    mesh_settings.numTriangles = static_cast<IPLint32>(wall.triangles.size());
    mesh_settings.numMaterials = static_cast<IPLint32>(wall.materials.size());
    mesh_settings.vertices = wall.vertices.data();
    mesh_settings.triangles = wall.triangles.data();
    mesh_settings.materialIndices = wall.material_indices.data();
    mesh_settings.materials = wall.materials.data();

    StaticMesh mesh;
    check(iplStaticMeshCreate(scene.get(), &mesh_settings, mesh.out()), "iplStaticMeshCreate");
    const MeshAttachment mesh_attachment(mesh.get(), scene.get());

    IPLSimulationSettings simulation_settings{};
    simulation_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    simulation_settings.sceneType = steam.scene_type();
    simulation_settings.maxNumOcclusionSamples = 16;
    simulation_settings.samplingRate = 48000;
    simulation_settings.frameSize = 256;

    Simulator simulator;
    check(iplSimulatorCreate(steam.context(), &simulation_settings, simulator.out()), "iplSimulatorCreate");
    iplSimulatorSetScene(simulator.get(), scene.get());

    IPLSourceSettings source_settings{};
    source_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    Source source;
    check(iplSourceCreate(simulator.get(), &source_settings, source.out()), "iplSourceCreate");
    const SourceAttachment source_attachment(source.get(), simulator.get());

    IPLSimulationSharedInputs shared{};
    shared.listener = pose_at({0.0f, 0.0f, -5.0f});
    iplSimulatorSetSharedInputs(simulator.get(), IPL_SIMULATIONFLAGS_DIRECT, &shared);

    vsa_self_test_report report{};
    report.struct_size = sizeof report;
    // Listener at z = -5; this source is behind the wall at z = +5.
    report.occlusion_through_wall = simulate_occlusion(simulator.get(), source.get(), {0.0f, 0.0f, 5.0f});
    // Same distance, but off to the side where the straight path misses the wall.
    report.occlusion_clear_path = simulate_occlusion(simulator.get(), source.get(), {6.0f, 0.0f, -5.0f});

    report.passed = report.occlusion_through_wall < 0.01f && report.occlusion_clear_path > 0.99f ? 1u : 0u;
    report.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return report;
}

}  // namespace vsa::steam
