// Paths through the air (ADR 0011): straight in the open, around a pillar, through a doorway,
// dearer through a door, none out of a sealed room.

#include "world/air_paths.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <memory>

using namespace vsa::world;

namespace {

enum : uint16_t { Air = 0, Stone = 1, Water = 2 };

std::vector<TransmissionMaterial> materials() {
    TransmissionMaterial air;
    TransmissionMaterial stone;
    stone.kind = MaterialKind::Solid;
    TransmissionMaterial water;
    water.kind = MaterialKind::Liquid;
    return {air, stone, water};
}

VoxelView view_of(const std::shared_ptr<ChunkVoxels>& c) {
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    return VoxelView(std::move(chunks), materials());
}

double straight(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/// A closed stone box around the interior [2, 14) on each axis.
std::shared_ptr<ChunkVoxels> room() {
    auto c = std::make_shared<ChunkVoxels>();
    for (int y = 1; y <= 14; ++y) {
        for (int z = 1; z <= 14; ++z) {
            for (int x = 1; x <= 14; ++x) {
                if (x == 1 || x == 14 || y == 1 || y == 14 || z == 1 || z == 14) {
                    c->materials[static_cast<std::size_t>(cell_index(x, y, z))] = Stone;
                }
            }
        }
    }
    return c;
}

}  // namespace

TEST_CASE("air paths: in the open, never shorter than the straight line and within 13 % of it") {
    const VoxelView view = view_of(std::make_shared<ChunkVoxels>());
    AirField field;
    const double from[3] = {10.5, 10.5, 10.5};
    field.compute(view, from);  // the first allocates
    const auto started = std::chrono::steady_clock::now();
    field.compute(view, from);  // every cell reachable: the most work
    MESSAGE("a search through open space takes "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << " ms");
    for (const auto& to : {std::array<double, 3>{20.5, 10.5, 10.5}, std::array<double, 3>{17.5, 17.5, 10.5},
                           std::array<double, 3>{16.5, 16.5, 16.5}, std::array<double, 3>{25.5, 14.5, 3.5}}) {
        const double d = field.distance(to.data());
        CAPTURE(to[0]);
        CHECK(d >= straight(from, to.data()));
        CHECK(d <= 1.13 * straight(from, to.data()));  // chamfer 3-4-5: up to ~12 % off-axis
    }
    const double far[3] = {10.5 + AirField::kRadius + 2, 10.5, 10.5};
    CHECK(field.distance(far) == AirField::kNone);
}

TEST_CASE("air paths: around a pillar, through a doorway, not out of a sealed room") {
    auto c = room();
    // A pillar between x 7 and 9, z 6..10, floor to ceiling.
    for (int y = 2; y <= 13; ++y) {
        for (int z = 6; z <= 10; ++z) {
            for (int x = 7; x <= 8; ++x) {
                c->materials[static_cast<std::size_t>(cell_index(x, y, z))] = Stone;
            }
        }
    }
    const VoxelView sealed = view_of(c);
    AirField field;
    const double from[3] = {4.5, 5.5, 8.5};
    field.compute(sealed, from);
    const double behind[3] = {11.5, 5.5, 8.5};
    const double around = field.distance(behind);
    MESSAGE("around the pillar: " << around << " m (straight " << straight(from, behind) << " m)");
    CHECK(around > straight(from, behind) + 1.0);
    CHECK(around < 12.0);
    const double outside[3] = {18.5, 5.5, 8.5};
    CHECK(field.distance(outside) == AirField::kNone);

    // A doorway in the east wall (x 14, z 8, y 2..3): outside is reachable, the long way.
    c->materials[static_cast<std::size_t>(cell_index(14, 2, 8))] = Air;
    c->materials[static_cast<std::size_t>(cell_index(14, 3, 8))] = Air;
    const VoxelView open = view_of(c);
    field.compute(open, from);
    const double through_doorway = field.distance(outside);
    MESSAGE("out through the doorway: " << through_doorway << " m");
    CHECK(through_doorway < AirField::kNone);
    CHECK(through_doorway > straight(from, outside));

    // A door in it (a partial block): passable, dearer.
    PartialBlock door;
    door.cell = static_cast<uint16_t>(cell_index(14, 2, 8));
    door.material = Stone;
    door.boxes.push_back(Box{{0.0f, 0.0f, 0.4f}, {1.0f, 1.0f, 0.6f}});
    c->partials.push_back(door);
    c->partials.push_back(door);
    c->partials.back().cell = static_cast<uint16_t>(cell_index(14, 3, 8));
    sort_partials(*c);
    const VoxelView door_view = view_of(c);
    field.compute(door_view, from);
    const double through_door = field.distance(outside);
    MESSAGE("out through the door: " << through_door << " m");
    CHECK(through_door > through_doorway + 3.0);
}

TEST_CASE("air paths: water and diagonal gaps between blocks let nothing through") {
    auto c = room();
    // A wall of water across the room at x 8 seals it.
    for (int y = 2; y <= 13; ++y) {
        for (int z = 2; z <= 13; ++z) {
            c->materials[static_cast<std::size_t>(cell_index(8, y, z))] = Water;
        }
    }
    const VoxelView view = view_of(c);
    AirField field;
    const double from[3] = {4.5, 5.5, 8.5};
    field.compute(view, from);
    const double beyond[3] = {11.5, 5.5, 8.5};
    CHECK(field.distance(beyond) == AirField::kNone);

    // A divider across the room at x 8..9 whose only openings, (8, y, 5) and (9, y, 6), touch at
    // an edge: sealed, since a diagonal step may not squeeze between the stone either side of it.
    auto d = room();
    for (int y = 2; y <= 13; ++y) {
        for (int z = 2; z <= 13; ++z) {
            for (int x = 8; x <= 9; ++x) {
                d->materials[static_cast<std::size_t>(cell_index(x, y, z))] = Stone;
            }
        }
        d->materials[static_cast<std::size_t>(cell_index(8, y, 5))] = Air;
        d->materials[static_cast<std::size_t>(cell_index(9, y, 6))] = Air;
    }
    const VoxelView diagonal = view_of(d);
    const double west[3] = {4.5, 5.5, 5.5};
    field.compute(diagonal, west);
    const double east[3] = {12.5, 5.5, 6.5};
    CHECK(field.distance(east) == AirField::kNone);
    // Open the corner (9, y, 5) and it passes.
    for (int y = 2; y <= 13; ++y) {
        d->materials[static_cast<std::size_t>(cell_index(9, y, 5))] = Air;
    }
    const VoxelView opened = view_of(d);
    field.compute(opened, west);
    CHECK(field.distance(east) < 12.0);
}
