// Phase 7's decision gate (ADR 0005, decided in ADR 0013): how long Steam Audio takes to bake
// pathing data for a 64 x 64 x 64 region of typical world (terrain, two buildings with doorways,
// a cave) on one thread, and how much memory the result takes. Measured: 794 probes, 0.5 s with
// one visibility sample per probe (2.1 s with four), 2.6 MB. Kept as the bake's budget.

#include "steam/ipl_handle.hpp"
#include "steam/steam_context.hpp"
#include "world/path_baker.hpp"
#include "world/world_scene.hpp"

#include "support/budgets.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace vsa::world;
using namespace std::chrono_literals;

namespace {

enum : uint16_t { Air = 0, Stone = 1, Soil = 2, Wood = 3 };
constexpr int kRegion = 64;

std::vector<AcousticMaterial> materials() {
    AcousticMaterial air;
    air.name = "air";
    air.kind = MaterialKind::Air;
    AcousticMaterial stone;
    stone.name = "stone";
    AcousticMaterial soil;
    soil.name = "soil";
    soil.absorption[0] = 0.3f;
    soil.absorption[1] = 0.45f;
    soil.absorption[2] = 0.6f;
    AcousticMaterial wood;
    wood.name = "wood";
    wood.absorption[0] = 0.2f;
    wood.absorption[1] = 0.3f;
    wood.absorption[2] = 0.33f;
    return {air, stone, soil, wood};
}

/// A region of typical world: bumpy terrain, a wooden house and a stone hall with doorways on
/// it, and a winding cave from the surface down to a chamber.
class Region {
public:
    Region() : cells_(static_cast<std::size_t>(kRegion) * kRegion * kRegion, Air) {
        for (int z = 0; z < kRegion; ++z) {
            for (int x = 0; x < kRegion; ++x) {
                const int h = height(x, z);
                for (int y = 0; y < h; ++y) {
                    set(x, y, z, y < h - 3 ? Stone : Soil);
                }
            }
        }
        house(8, 17, 8, 17, 30, Wood, 4, 8, 12);
        house(40, 51, 40, 51, 30, Stone, 6, 51, 45);
        cave();
    }

    [[nodiscard]] std::shared_ptr<ChunkVoxels> chunk(int cx, int cy, int cz) const {
        auto c = std::make_shared<ChunkVoxels>();
        for (int y = 0; y < kChunkSize; ++y) {
            for (int z = 0; z < kChunkSize; ++z) {
                for (int x = 0; x < kChunkSize; ++x) {
                    c->materials[static_cast<std::size_t>(cell_index(x, y, z))] =
                        get(cx * kChunkSize + x, cy * kChunkSize + y, cz * kChunkSize + z);
                }
            }
        }
        return c;
    }

    [[nodiscard]] int air_cells() const {
        int n = 0;
        for (const uint16_t m : cells_) {
            n += m == Air ? 1 : 0;
        }
        return n;
    }

private:
    static int height(int x, int z) {
        return 28 + static_cast<int>(std::lround(4.0 * std::sin(x / 7.0) + 3.0 * std::cos(z / 5.0) + 2.0 * std::sin((x + z) / 3.0)));
    }
    void set(int x, int y, int z, uint16_t m) {
        if (x >= 0 && y >= 0 && z >= 0 && x < kRegion && y < kRegion && z < kRegion) {
            cells_[(static_cast<std::size_t>(y) * kRegion + static_cast<std::size_t>(z)) * kRegion + static_cast<std::size_t>(x)] = m;
        }
    }
    [[nodiscard]] uint16_t get(int x, int y, int z) const {
        return cells_[(static_cast<std::size_t>(y) * kRegion + static_cast<std::size_t>(z)) * kRegion + static_cast<std::size_t>(x)];
    }
    /// A building with its floor at `floor`, walls `tall` high, a two-block doorway at (dx, dz).
    void house(int x0, int x1, int z0, int z1, int floor, uint16_t wall, int tall, int dx, int dz) {
        for (int z = z0 - 1; z <= z1 + 1; ++z) {
            for (int x = x0 - 1; x <= x1 + 1; ++x) {
                for (int y = floor - 1; y <= floor + tall; ++y) {
                    const bool shell = x == x0 - 1 || x == x1 + 1 || z == z0 - 1 || z == z1 + 1 || y == floor - 1 || y == floor + tall;
                    set(x, y, z, shell ? wall : static_cast<uint16_t>(Air));
                }
                for (int y = floor + tall + 1; y < kRegion; ++y) {
                    set(x, y, z, Air);  // nothing above the roof
                }
                for (int y = 0; y < floor - 1; ++y) {
                    set(x, y, z, get(x, y, z) == Air ? static_cast<uint16_t>(Stone) : get(x, y, z));  // solid ground under it
                }
            }
        }
        set(dx, floor, dz, Air);
        set(dx, floor + 1, dz, Air);
        set(dx, floor, dz + 1, Air);
        set(dx, floor + 1, dz + 1, Air);
    }
    void cave() {
        uint32_t state = 12345;
        const auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<int>(state >> 30);  // 0..3
        };
        int x = 30;
        int z = 5;
        int y = height(x, z) - 1;
        for (int step = 0; step < 70; ++step) {
            for (int dy = 0; dy <= 2; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        set(x + dx, y + dy, z + dz, Air);
                    }
                }
            }
            const int r = next();
            if (r == 0 && x < 55) {
                ++x;
            } else if (r == 1 && x > 8) {
                --x;
            } else if (z < 58) {
                ++z;
            }
            if (step % 3 == 0 && y > 8) {
                --y;
            }
        }
        for (int dy = 0; dy < 5; ++dy) {
            for (int dz = -5; dz <= 5; ++dz) {
                for (int dx = -5; dx <= 5; ++dx) {
                    set(x + dx, y + dy, z + dz, Air);  // a chamber at the end
                }
            }
        }
    }

    std::vector<uint16_t> cells_;
};

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

// Steam Audio 4.8.1's path baker calls the progress callback unconditionally (BakedPathData's
// constructor), although the header calls it optional: passing none crashes. Always pass one.
TEST_CASE("pathing: bakes on a plain static-mesh box") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_STEAM, false});
    IPLSceneSettings settings = steam.scene_settings();
    vsa::steam::Scene scene;
    REQUIRE(iplSceneCreate(steam.context(), &settings, scene.out()) == IPL_STATUS_SUCCESS);
    // A 20 x 4 x 20 room: floor at y 0, ceiling at y 4, walls; inward-facing triangles.
    std::vector<IPLVector3> v;
    std::vector<IPLTriangle> t;
    std::vector<IPLint32> mi;
    const auto quad = [&](IPLVector3 a, IPLVector3 b, IPLVector3 c, IPLVector3 d) {
        const auto base = static_cast<IPLint32>(v.size());
        v.insert(v.end(), {a, b, c, d});
        t.push_back({{base, base + 1, base + 2}});
        t.push_back({{base, base + 2, base + 3}});
        mi.push_back(0);
        mi.push_back(0);
    };
    quad({0, 0, 0}, {0, 0, 20}, {20, 0, 20}, {20, 0, 0});    // floor
    quad({0, 4, 0}, {20, 4, 0}, {20, 4, 20}, {0, 4, 20});    // ceiling
    quad({0, 0, 0}, {20, 0, 0}, {20, 4, 0}, {0, 4, 0});      // z = 0
    quad({0, 0, 20}, {0, 4, 20}, {20, 4, 20}, {20, 0, 20});  // z = 20
    quad({0, 0, 0}, {0, 4, 0}, {0, 4, 20}, {0, 0, 20});      // x = 0
    quad({20, 0, 0}, {20, 0, 20}, {20, 4, 20}, {20, 4, 0});  // x = 20
    IPLMaterial material{{0.1f, 0.1f, 0.1f}, 0.05f, {0.0f, 0.0f, 0.0f}};
    IPLStaticMeshSettings mesh{};
    mesh.numVertices = static_cast<IPLint32>(v.size());
    mesh.numTriangles = static_cast<IPLint32>(t.size());
    mesh.numMaterials = 1;
    mesh.vertices = v.data();
    mesh.triangles = t.data();
    mesh.materialIndices = mi.data();
    mesh.materials = &material;
    vsa::steam::StaticMesh static_mesh;
    REQUIRE(iplStaticMeshCreate(scene.get(), &mesh, static_mesh.out()) == IPL_STATUS_SUCCESS);
    iplStaticMeshAdd(static_mesh.get(), scene.get());
    iplSceneCommit(scene.get());

    IPLProbeGenerationParams generation{};
    generation.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    generation.spacing = 2.5f;
    generation.height = 1.6f;
    generation.transform.elements[0][0] = 20.0f;
    generation.transform.elements[1][1] = 4.0f;
    generation.transform.elements[2][2] = 20.0f;
    generation.transform.elements[3][3] = 1.0f;
    generation.transform.elements[0][3] = 10.0f;
    generation.transform.elements[1][3] = 2.0f;
    generation.transform.elements[2][3] = 10.0f;
    vsa::steam::ProbeArray array;
    REQUIRE(iplProbeArrayCreate(steam.context(), array.out()) == IPL_STATUS_SUCCESS);
    iplProbeArrayGenerateProbes(array.get(), scene.get(), &generation);
    MESSAGE("box: " << iplProbeArrayGetNumProbes(array.get()) << " probes");
    vsa::steam::ProbeBatch batch;
    REQUIRE(iplProbeBatchCreate(steam.context(), batch.out()) == IPL_STATUS_SUCCESS);
    iplProbeBatchAddProbeArray(batch.get(), array.get());
    iplProbeBatchCommit(batch.get());
    IPLBakedDataIdentifier identifier{};
    identifier.type = IPL_BAKEDDATATYPE_PATHING;
    identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    IPLPathBakeParams bake{};
    bake.scene = scene.get();
    bake.probeBatch = batch.get();
    bake.identifier = identifier;
    bake.numSamples = 1;
    bake.radius = 1.0f;
    bake.threshold = 0.1f;
    bake.visRange = 32.0f;
    bake.pathRange = 64.0f;
    bake.numThreads = 1;
    iplPathBakerBake(steam.context(), &bake, [](IPLfloat32, void*) {}, nullptr);  // 4.8.1 needs a callback
    MESSAGE("box pathing data " << static_cast<double>(iplProbeBatchGetDataSize(batch.get(), &identifier)) / 1024.0 << " KB");
    iplStaticMeshRemove(static_mesh.get(), scene.get());
    iplSceneCommit(scene.get());
}

TEST_CASE("pathing: the probe budget bounds the bake, whatever the terrain holds (ADR 0015)") {
    // The baker widens the spacing until the box holds no more than the budget, because a bake
    // costs about probes^2.2: open ground held 3859 probes at 2.5 m and took 16.7 s in game.
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_STEAM, false});
    WorldScene scene(steam);
    scene.set_materials(materials());
    const Region region;
    for (int cy = 0; cy < 2; ++cy) {
        for (int cz = 0; cz < 2; ++cz) {
            for (int cx = 0; cx < 2; ++cx) {
                scene.set_chunk({cx, cy, cz}, region.chunk(cx, cy, cz), 0);
            }
        }
    }
    REQUIRE(scene.wait_idle(30s));

    vsa::ListenerPose pose{};
    pose.position[0] = kRegion / 2.0f;
    pose.position[1] = kRegion / 2.0f;
    pose.position[2] = kRegion / 2.0f;

    // Unbudgeted (a budget nothing reaches) against the shipping budget, same box.
    uint32_t loose_probes = 0;
    double loose_ms = 0.0;
    for (const uint32_t budget : {65536u, 1200u, 300u}) {
        PathBakeSettings settings;
        settings.range = 64;
        settings.height = 64;
        settings.max_probes = budget;
        PathBaker baker(steam, scene, settings);
        baker.set_listener(pose);
        baker.offline_tick(0.0);
        const PathBakeStats stats = baker.stats();
        REQUIRE(stats.bakes == 1);
        MESSAGE("budget " << budget << ": " << stats.probes << " probes " << stats.spacing << " m apart in "
                << stats.last_bake_ms << " ms");
        CHECK(stats.probes > 100);          // the region really is baked
        CHECK(stats.probes <= budget);      // and within its budget
        CHECK(stats.cancelled == 0);
        if (budget == 65536u) {
            loose_probes = stats.probes;
            loose_ms = stats.last_bake_ms;
            CHECK(stats.spacing == doctest::Approx(settings.spacing));  // untouched when it fits
        } else if (loose_probes > budget) {
            // The budget bit: wider probes, and a bake that is faster by more than the ratio of
            // probes, since the cost is superlinear.
            CHECK(stats.spacing > settings.spacing);
            CHECK(stats.last_bake_ms < loose_ms);
        }
    }
}

TEST_CASE("pathing: a listener that keeps moving abandons bakes without corrupting anything") {
    // A bake the listener has walked out of is thrown away when it finishes; it is never cut
    // short, because Steam Audio 4.8.1's cancel poisons its thread pool (ADR 0017) and the game
    // then dies seconds later, somewhere else. This walks a listener far enough, often enough,
    // to abandon bake after bake: it crashed within seconds while the bake was being cancelled.
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_STEAM, false});
    WorldScene scene(steam);
    scene.set_materials(materials());
    const Region region;
    for (int cy = 0; cy < 2; ++cy) {
        for (int cz = 0; cz < 2; ++cz) {
            for (int cx = 0; cx < 2; ++cx) {
                scene.set_chunk({cx, cy, cz}, region.chunk(cx, cy, cz), 0);
            }
        }
    }
    REQUIRE(scene.wait_idle(30s));

    PathBakeSettings settings;
    settings.range = 64;
    settings.height = 64;
    PathBaker baker(steam, scene, settings);
    vsa::ListenerPose pose{};
    pose.position[1] = kRegion / 2.0f;
    baker.set_listener(pose);
    baker.set_threaded(true);

    // Walk from one end of the region to the other and back, a stride at a time. Each leg is
    // more than a third of the box, so every bake in flight is abandoned.
    const auto started = std::chrono::steady_clock::now();
    for (int step = 0; std::chrono::steady_clock::now() - started < 6s; ++step) {
        const float t = static_cast<float>(step % 20) / 19.0f;
        pose.position[0] = 8.0f + t * (kRegion - 16.0f);
        pose.position[2] = kRegion / 2.0f;
        baker.set_listener(pose);
        std::this_thread::sleep_for(60ms);
    }
    baker.set_threaded(false);

    const PathBakeStats stats = baker.stats();
    MESSAGE("moving: " << stats.bakes << " bakes, " << stats.cancelled << " abandoned, last " << stats.last_bake_ms << " ms");
    CHECK_FALSE(stats.baking);             // nothing left running
    if (stats.bakes + stats.cancelled > 0) {
        CHECK(stats.cancelled > 0);        // the point of the exercise
    } else {
        // Nothing finished inside the six seconds, so there was nothing to walk away from.
        // Steam Audio's own ray tracer bakes this region in about half a second on x64 and in
        // about five minutes on Apple Silicon, where this is what the counters look like. The
        // crash ADR 0017 is about would still have happened by now, so the test is not worthless
        // here -- only its last two lines are.
        MESSAGE("no bake finished inside the window: abandonment not exercised on this machine");
    }
    // Whatever it settled on is a whole batch, or none at all.
    const std::shared_ptr<const PathBatch> batch = baker.current();
    if (batch) {
        CHECK(batch->probes > 0);
        CHECK(batch->probes <= settings.max_probes);
    }
}

TEST_CASE("pathing bake spike: a 64 x 64 x 64 region of terrain, buildings and a cave on one thread") {
    vsa::steam::SteamContext steam({VSA_RAY_TRACER_STEAM, false});
    WorldScene scene(steam);
    scene.set_materials(materials());
    const Region region;
    for (int cy = 0; cy < 2; ++cy) {
        for (int cz = 0; cz < 2; ++cz) {
            for (int cx = 0; cx < 2; ++cx) {
                scene.set_chunk({cx, cy, cz}, region.chunk(cx, cy, cz), 0);
            }
        }
    }
    REQUIRE(scene.wait_idle(30s));
    const SceneStats stats = scene.stats();
    MESSAGE("region: " << region.air_cells() << " air cells, " << stats.triangles << " triangles in " << stats.meshed_chunks << " chunks");

    std::shared_lock lock(scene.scene_lock());
    const IPLScene ipl_scene = scene.scene_locked();
    REQUIRE(ipl_scene != nullptr);

    // Probes 2.5 m apart, 1.6 m above every floor (the plan's spacing and height).
    IPLProbeGenerationParams generation{};
    generation.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    generation.spacing = 2.5f;
    generation.height = 1.6f;
    generation.transform.elements[0][0] = static_cast<float>(kRegion);
    generation.transform.elements[1][1] = static_cast<float>(kRegion);
    generation.transform.elements[2][2] = static_cast<float>(kRegion);
    generation.transform.elements[3][3] = 1.0f;
    // Steam Audio samples the box centred on the transform's translation (its unit cube is -0.5..0.5).
    generation.transform.elements[0][3] = kRegion / 2.0f;
    generation.transform.elements[1][3] = kRegion / 2.0f;
    generation.transform.elements[2][3] = kRegion / 2.0f;
    vsa::steam::ProbeArray array;
    REQUIRE(iplProbeArrayCreate(steam.context(), array.out()) == IPL_STATUS_SUCCESS);
    auto started = std::chrono::steady_clock::now();
    iplProbeArrayGenerateProbes(array.get(), ipl_scene, &generation);
    const double generate_ms = ms_since(started);
    const IPLint32 probes = iplProbeArrayGetNumProbes(array.get());
    MESSAGE("probes: " << probes << " generated in " << generate_ms << " ms");
    CHECK(probes > 100);

    vsa::steam::ProbeBatch batch;
    REQUIRE(iplProbeBatchCreate(steam.context(), batch.out()) == IPL_STATUS_SUCCESS);
    iplProbeBatchAddProbeArray(batch.get(), array.get());
    iplProbeBatchCommit(batch.get());

    IPLBakedDataIdentifier identifier{};
    identifier.type = IPL_BAKEDDATATYPE_PATHING;
    identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
    for (const IPLint32 samples : {1, 4}) {
        IPLPathBakeParams bake{};
        bake.scene = ipl_scene;
        bake.probeBatch = batch.get();
        bake.identifier = identifier;
        bake.numSamples = samples;
        bake.radius = 1.0f;
        bake.threshold = 0.1f;
        bake.visRange = 32.0f;
        bake.pathRange = 64.0f;
        bake.numThreads = 1;
        started = std::chrono::steady_clock::now();
        iplPathBakerBake(steam.context(), &bake, [](IPLfloat32, void*) {}, nullptr);  // 4.8.1 needs a callback
        const double bake_ms = ms_since(started);
        const IPLsize bytes = iplProbeBatchGetDataSize(batch.get(), &identifier);
        MESSAGE("bake with " << samples << " visibility sample(s) per probe, 1 thread: " << bake_ms << " ms; pathing data "
                             << static_cast<double>(bytes) / 1024.0 << " KB");
        CHECK(bytes < 8u * 1024u * 1024u);
#if defined(NDEBUG)
        if (samples == 1 && vsa_test::perf_budgets_enforced()) {
            CHECK(bake_ms < 2000.0);  // ADR 0005's gate
        }
#endif
    }
}
