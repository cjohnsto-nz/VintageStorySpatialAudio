// The world scene: chunk snapshots become Steam Audio geometry at the right place (checked with
// real occlusion queries), relative to a movable origin, and follow edits and removals.

#include "steam/ipl_handle.hpp"
#include "steam/steam_context.hpp"
#include "world/world_scene.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace vsa::world;
using namespace std::chrono_literals;

namespace {

enum : uint16_t { Air = 0, Stone = 1 };

std::vector<AcousticMaterial> materials() {
    AcousticMaterial air;
    air.name = "air";
    air.kind = MaterialKind::Air;
    AcousticMaterial stone;
    stone.name = "stone";
    stone.kind = MaterialKind::Solid;
    return {air, stone};
}

/// A one-block-thick stone wall across the chunk at local x = 16.
std::shared_ptr<ChunkVoxels> wall_chunk() {
    auto c = std::make_shared<ChunkVoxels>();
    for (int y = 0; y < kChunkSize; ++y) {
        for (int z = 0; z < kChunkSize; ++z) {
            c->materials[static_cast<std::size_t>(cell_index(16, y, z))] = Stone;
        }
    }
    return c;
}

/// Occlusion between two points (scene coordinates, i.e. relative to the origin) by ray cast.
class Probe {
public:
    Probe(const vsa::steam::SteamContext& steam, WorldScene& scene) : scene_(scene) {
        IPLSimulationSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        settings.sceneType = steam.scene_type();
        settings.maxNumOcclusionSamples = 1;
        settings.samplingRate = 48000;
        settings.frameSize = 256;
        REQUIRE(iplSimulatorCreate(steam.context(), &settings, simulator_.out()) == IPL_STATUS_SUCCESS);
        scene.attach(simulator_.get());
        IPLSourceSettings source_settings{};
        source_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        REQUIRE(iplSourceCreate(simulator_.get(), &source_settings, source_.out()) == IPL_STATUS_SUCCESS);
        iplSourceAdd(source_.get(), simulator_.get());
        iplSimulatorCommit(simulator_.get());
    }
    ~Probe() {
        iplSourceRemove(source_.get(), simulator_.get());
        iplSimulatorCommit(simulator_.get());
        scene_.detach(simulator_.get());
    }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    float occlusion(IPLVector3 listener, IPLVector3 source) {
        std::lock_guard lock(scene_.scene_lock());
        IPLSimulationSharedInputs shared{};
        shared.listener = pose(listener);
        iplSimulatorSetSharedInputs(simulator_.get(), IPL_SIMULATIONFLAGS_DIRECT, &shared);
        IPLSimulationInputs inputs{};
        inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
        inputs.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
        inputs.source = pose(source);
        inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
        iplSourceSetInputs(source_.get(), IPL_SIMULATIONFLAGS_DIRECT, &inputs);
        iplSimulatorRunDirect(simulator_.get());
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(source_.get(), IPL_SIMULATIONFLAGS_DIRECT, &outputs);
        return outputs.direct.occlusion;
    }

private:
    static IPLCoordinateSpace3 pose(IPLVector3 origin) {
        IPLCoordinateSpace3 p{};
        p.right = {1, 0, 0};
        p.up = {0, 1, 0};
        p.ahead = {0, 0, -1};
        p.origin = origin;
        return p;
    }

    WorldScene& scene_;
    vsa::steam::Simulator simulator_;
    vsa::steam::Source source_;
};

}  // namespace

TEST_CASE("world scene: a chunk's wall occludes exactly where the world puts it, relative to the origin") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_AUTO, false});
    WorldScene scene(steam);
    scene.set_materials(materials());
    const ChunkKey key{16000, 3, -8000};  // around Vintage Story's usual ~512 000 block coordinates
    scene.set_origin(key.x * kChunkSize, key.y * kChunkSize, key.z * kChunkSize);
    scene.set_chunk(key, wall_chunk(), 0);
    REQUIRE(scene.wait_idle(10s));

    const SceneStats stats = scene.stats();
    CHECK(stats.chunks == 1);
    CHECK(stats.meshed_chunks == 1);
    CHECK(stats.triangles == 4);  // the wall's two faces (its edges face unloaded chunks)
    CHECK(stats.pending_chunks == 0);

    Probe probe(steam, scene);
    // The wall spans local x 16..17: scene x 16..17 with this origin.
    CHECK(probe.occlusion({10, 16, 16}, {24, 16, 16}) < 0.01f);
    CHECK(probe.occlusion({10, 16, 16}, {10, 16, 26}) > 0.99f);
    CHECK(probe.occlusion({18, 5, 5}, {30, 20, 20}) > 0.99f);  // both on the same side

    // Moving the origin by 32 blocks in x moves the wall to scene x -16..-15.
    scene.set_origin((key.x + 1) * kChunkSize, key.y * kChunkSize, key.z * kChunkSize);
    REQUIRE(scene.wait_idle(10s));
    CHECK(probe.occlusion({-20, 16, 16}, {-10, 16, 16}) < 0.01f);
    CHECK(probe.occlusion({10, 16, 16}, {24, 16, 16}) > 0.99f);

    // Removed: no wall.
    scene.remove_chunk(key);
    REQUIRE(scene.wait_idle(10s));
    CHECK(scene.stats().chunks == 0);
    CHECK(scene.stats().triangles == 0);
    CHECK(probe.occlusion({-20, 16, 16}, {-10, 16, 16}) > 0.99f);
}

TEST_CASE("world scene: a neighbour's arrival re-meshes the shared border; empty chunks have no geometry") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_AUTO, false});
    WorldScene scene(steam);
    scene.set_materials(materials());

    auto a = std::make_shared<ChunkVoxels>();
    a->materials[static_cast<std::size_t>(cell_index(31, 0, 0))] = Stone;
    scene.set_chunk({0, 0, 0}, a, 0);
    REQUIRE(scene.wait_idle(10s));
    // Faces only towards cells inside the chunk (-x, +y, +z); the other three face unknown chunks.
    CHECK(scene.stats().triangles == 6);

    // Air chunk arrives on +x: the block's +x face appears.
    scene.set_chunk({1, 0, 0}, std::make_shared<ChunkVoxels>(), 0);
    REQUIRE(scene.wait_idle(10s));
    std::shared_ptr<const ChunkMesh> mesh;
    int lod = -1;
    REQUIRE(scene.chunk_mesh({0, 0, 0}, mesh, lod));
    CHECK(lod == 0);
    const auto triangles_with_air = mesh->triangle_count();
    CHECK(triangles_with_air == 8);
    CHECK(!scene.chunk_mesh({1, 0, 0}, mesh, lod));  // all air: nothing to mesh

    // Replaced by a solid one: the face is buried again.
    auto solid = std::make_shared<ChunkVoxels>();
    solid->materials[static_cast<std::size_t>(cell_index(0, 0, 0))] = Stone;
    scene.set_chunk({1, 0, 0}, solid, 0);
    REQUIRE(scene.wait_idle(10s));
    REQUIRE(scene.chunk_mesh({0, 0, 0}, mesh, lod));
    CHECK(mesh->triangle_count() + 2 == triangles_with_air);
    CHECK(scene.chunk_keys().size() == 2);

    // OBJ export: world coordinates, one group per meshed chunk.
    const auto path = (std::filesystem::temp_directory_path() / "vsaudio-scene-test.obj").string();
    scene.save_obj(path);
    std::ifstream obj(path);
    std::string line;
    int vertices = 0;
    int groups = 0;
    while (std::getline(obj, line)) {
        vertices += line.rfind("v ", 0) == 0 ? 1 : 0;
        groups += line.rfind("g ", 0) == 0 ? 1 : 0;
    }
    CHECK(groups == 2);
    CHECK(vertices > 0);
    obj.close();
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path).replace_extension(".mtl"));
}

TEST_CASE("world scene: new materials re-mesh everything, and the destructor releases a busy scene") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_AUTO, false});
    auto scene = std::make_unique<WorldScene>(steam);
    for (int x = 0; x < 4; ++x) {
        scene->set_chunk({x, 0, 0}, wall_chunk(), x % 2);
    }
    REQUIRE(scene->wait_idle(10s));
    CHECK(scene->stats().meshed_chunks == 0);  // no materials yet: everything counts as air
    scene->set_materials(materials());
    REQUIRE(scene->wait_idle(10s));
    CHECK(scene->stats().meshed_chunks == 4);
    for (int x = 4; x < 40; ++x) {
        scene->set_chunk({x, 0, 0}, wall_chunk(), 0);
    }
    scene.reset();  // mid-work: must stop cleanly, releasing everything (ASan/LSan in CI)
}

TEST_CASE("world scene: rebuilding the top-level scene stays cheap with a realistic chunk count") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_AUTO, false});
    WorldScene scene(steam);
    scene.set_materials(materials());
    for (int x = 0; x < 9; ++x) {
        for (int z = 0; z < 9; ++z) {
            for (int y = 0; y < 5; ++y) {
                scene.set_chunk({x, y, z}, wall_chunk(), (std::abs(x - 4) > 2 || std::abs(z - 4) > 2) ? 1 : 0);
            }
        }
    }
    REQUIRE(scene.wait_idle(60s));
    const SceneStats before = scene.stats();
    CHECK(before.meshed_chunks == 405);
    scene.set_chunk({4, 2, 4}, wall_chunk(), 0);  // one edit: a fresh top-level scene of 405 instances
    REQUIRE(scene.wait_idle(10s));
    const SceneStats after = scene.stats();
    MESSAGE("top-level scene of " << after.meshed_chunks << " instances rebuilt in " << after.last_commit_ms << " ms");
    CHECK(after.last_commit_ms < 150.0);  // ~25 ms in Release; built with no lock held
}
