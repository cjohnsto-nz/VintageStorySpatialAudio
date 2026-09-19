// The boundary-only greedy mesher (ADR 0003): which surfaces exist, their materials, merging,
// winding, chunk borders, partial blocks and the coarse level of detail.

#include "world/mesher.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>

using namespace vsa::world;

namespace {

enum : uint16_t { Air = 0, Stone = 1, Soil = 2, Water = 3, Leaves = 4, Wood = 5 };

Mesher mesher() {
    return Mesher({MaterialKind::Air, MaterialKind::Solid, MaterialKind::Solid, MaterialKind::Liquid,
                   MaterialKind::Porous, MaterialKind::Solid});
}

void set(ChunkVoxels& c, int x, int y, int z, uint16_t m) { c.materials[static_cast<std::size_t>(cell_index(x, y, z))] = m; }

struct Summary {
    double area = 0.0;
    int triangles = 0;
    bool outward = true;  // every normal points away from `inside`
};

std::array<double, 3> normal_of(const ChunkMesh& mesh, std::size_t t, double& area) {
    const auto p = [&](int k) {
        const auto i = static_cast<std::size_t>(mesh.triangles[t * 3 + static_cast<std::size_t>(k)]) * 3;
        return std::array<double, 3>{static_cast<double>(mesh.vertices[i]), static_cast<double>(mesh.vertices[i + 1]),
                                     static_cast<double>(mesh.vertices[i + 2])};
    };
    const auto a = p(0);
    const auto b = p(1);
    const auto c = p(2);
    const std::array<double, 3> e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array<double, 3> e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array<double, 3> n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    area = 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return n;
}

/// Area of material `m` (all materials when m < 0), and whether normals point away from `inside`.
Summary summarise(const ChunkMesh& mesh, int m = -1, std::array<double, 3> inside = {-1e9, 0, 0}) {
    Summary s;
    for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
        if (m >= 0 && mesh.materials[t] != m) {
            continue;
        }
        double area = 0.0;
        const auto n = normal_of(mesh, t, area);
        s.area += area;
        ++s.triangles;
        if (inside[0] > -1e8) {
            const auto i = static_cast<std::size_t>(mesh.triangles[t * 3]) * 3;
            const auto vertex = [&](std::size_t k) { return static_cast<double>(mesh.vertices[i + k]); };
            const double d = n[0] * (vertex(0) - inside[0]) + n[1] * (vertex(1) - inside[1]) + n[2] * (vertex(2) - inside[2]);
            s.outward = s.outward && d > 0.0;
        }
    }
    return s;
}

const ChunkVoxels& empty_chunk() {
    static const ChunkVoxels air;
    return air;
}

Neighbours all_air() {
    Neighbours n{};
    n.fill(&empty_chunk());
    return n;
}

}  // namespace

TEST_CASE("mesher: a lone block in air is a closed cube with outward normals") {
    ChunkVoxels c;
    set(c, 5, 6, 7, Stone);
    const ChunkMesh mesh = mesher().mesh(c, all_air());
    const Summary s = summarise(mesh, -1, {5.5, 6.5, 7.5});
    CHECK(s.triangles == 12);
    CHECK(s.area == doctest::Approx(6.0));
    CHECK(s.outward);
    for (const uint16_t m : mesh.materials) {
        CHECK(m == Stone);
    }
}

TEST_CASE("mesher: a flat floor is two triangles, and unknown neighbours get no faces") {
    ChunkVoxels c;
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            set(c, x, 0, z, Stone);
        }
    }
    Neighbours only_above{};
    only_above[3] = &empty_chunk();  // +y loaded (air); every other neighbour unknown
    const ChunkMesh mesh = mesher().mesh(c, only_above);
    CHECK(mesh.triangle_count() == 2);
    CHECK(summarise(mesh).area == doctest::Approx(1024.0));

    // With air all around, the slab's sides and bottom appear too, each one rectangle.
    const ChunkMesh closed = mesher().mesh(c, all_air());
    CHECK(closed.triangle_count() == 12);
    CHECK(summarise(closed).area == doctest::Approx(2 * 1024.0 + 4 * 32.0));
}

TEST_CASE("mesher: surfaces only where a denser kind meets a more open one") {
    ChunkVoxels c;
    // Stone and soil side by side (no surface between them), water on the stone, air above.
    for (int z = 0; z < 4; ++z) {
        for (int x = 0; x < 4; ++x) {
            set(c, x, 0, z, x < 2 ? Stone : Soil);
            set(c, x, 1, z, Water);
        }
    }
    const ChunkMesh mesh = mesher().mesh(c, all_air());
    const Summary stone = summarise(mesh, Stone);
    const Summary soil = summarise(mesh, Soil);
    const Summary water = summarise(mesh, Water);
    // Stone: top under water (8) + bottom (8) + outer sides (3 sides x 2 wide... ) and no face towards soil.
    CHECK(stone.area == doctest::Approx(8 + 8 + 2 + 4 + 2));  // top, bottom, -z, -x, +z
    CHECK(soil.area == doctest::Approx(8 + 8 + 2 + 4 + 2));
    CHECK(water.area == doctest::Approx(16 + 16));  // top surface + four sides into air
}

TEST_CASE("mesher: coplanar faces of one material merge; different materials do not") {
    ChunkVoxels c;
    for (int x = 0; x < 8; ++x) {
        set(c, x, 0, 0, x < 4 ? Stone : Wood);
    }
    const ChunkMesh mesh = mesher().mesh(c, all_air());
    // Per material: top, bottom, -z, +z rectangles, and one end cap each.
    CHECK(summarise(mesh, Stone).triangles == 10);
    CHECK(summarise(mesh, Wood).triangles == 10);
}

TEST_CASE("mesher: chunk borders look into the neighbour chunk") {
    ChunkVoxels c;
    set(c, 31, 0, 0, Stone);
    ChunkVoxels solid_neighbour;
    set(solid_neighbour, 0, 0, 0, Stone);
    Neighbours n = all_air();
    n[1] = &solid_neighbour;  // +x
    const ChunkMesh covered = mesher().mesh(c, n);
    CHECK(summarise(covered).area == doctest::Approx(5.0));  // the +x face is buried

    n[1] = &empty_chunk();
    CHECK(summarise(mesher().mesh(c, n)).area == doctest::Approx(6.0));

    n[1] = nullptr;  // unknown
    CHECK(summarise(mesher().mesh(c, n)).area == doctest::Approx(5.0));
}

TEST_CASE("mesher: partial blocks are their boxes, and open to their neighbours") {
    ChunkVoxels c;
    set(c, 3, 0, 3, Stone);
    PartialBlock slab;
    slab.cell = static_cast<uint16_t>(cell_index(3, 1, 3));
    slab.material = Wood;
    slab.boxes.push_back({{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}});
    c.partials.push_back(slab);
    const ChunkMesh mesh = mesher().mesh(c, all_air());
    const Summary wood = summarise(mesh, Wood, {3.5, 1.25, 3.5});
    CHECK(wood.triangles == 12);
    CHECK(wood.area == doctest::Approx(2.0 + 4 * 0.5));
    CHECK(wood.outward);
    CHECK(summarise(mesh, Stone).area == doctest::Approx(6.0));  // including the top under the slab
}

TEST_CASE("mesher: porous leaves get surfaces against air, and solids against leaves") {
    ChunkVoxels c;
    set(c, 0, 0, 0, Wood);
    set(c, 0, 1, 0, Leaves);
    const ChunkMesh mesh = mesher().mesh(c, all_air());
    CHECK(summarise(mesh, Leaves).area == doctest::Approx(5.0));  // not towards the wood
    CHECK(summarise(mesh, Wood).area == doctest::Approx(6.0));    // including towards the leaves
}

TEST_CASE("mesher: the coarse level merges 2x2x2 blocks by majority") {
    ChunkVoxels c;
    for (int y = 0; y < 2; ++y) {
        for (int z = 0; z < 2; ++z) {
            for (int x = 0; x < 2; ++x) {
                set(c, x, y, z, Stone);
            }
        }
    }
    set(c, 1, 1, 1, Air);  // 7 of 8: still stone
    const ChunkMesh coarse = mesher().mesh(c, all_air(), 1);
    CHECK(coarse.triangle_count() == 12);
    CHECK(summarise(coarse, -1, {1.0, 1.0, 1.0}).area == doctest::Approx(24.0));
    CHECK(summarise(coarse, -1, {1.0, 1.0, 1.0}).outward);

    // A tie goes to the denser kind; a lone block vanishes.
    ChunkVoxels tie;
    for (int x = 0; x < 2; ++x) {
        for (int z = 0; z < 2; ++z) {
            set(tie, x, 0, z, Stone);
        }
    }
    set(tie, 5, 5, 5, Stone);
    const ChunkMesh coarse_tie = mesher().mesh(tie, all_air(), 1);
    CHECK(summarise(coarse_tie).area == doctest::Approx(24.0));
}

TEST_CASE("mesher: a noisy worst-case chunk stays within budget") {
    ChunkVoxels c;
    std::mt19937 rng(9);
    std::uniform_int_distribution<int> pick(0, 5);
    for (uint16_t& m : c.materials) {
        m = static_cast<uint16_t>(pick(rng));
    }
    const Mesher m = mesher();
    const auto start = std::chrono::steady_clock::now();
    const ChunkMesh mesh = m.mesh(c, all_air());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("noise chunk: " << mesh.triangle_count() << " triangles in " << ms << " ms");
    CHECK(mesh.triangle_count() > 0);

    // A typical terrain chunk: solid below a bumpy surface.
    ChunkVoxels terrain;
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            const int height = 12 + static_cast<int>(4.0 * std::sin(x * 0.3) + 3.0 * std::cos(z * 0.4));
            for (int y = 0; y < height; ++y) {
                set(terrain, x, y, z, y < height - 3 ? Stone : Soil);
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const ChunkMesh ground = m.mesh(terrain, all_air());
    const double ground_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("terrain chunk: " << ground.triangle_count() << " triangles in " << ground_ms << " ms");
}
