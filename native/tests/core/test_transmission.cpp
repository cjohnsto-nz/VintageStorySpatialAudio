// Voxel transmission (ADR 0004): losses per crossing and per metre, partial boxes, the thickness
// matrix (Phase 5's exit criterion), and sources escaping their own block.

#include "world/transmission.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace vsa::world;

namespace {

enum : uint16_t { Air = 0, Stone = 1, Wood = 2, Glass = 3, Leaves = 4 };

/// Losses as the engine derives them from the Phase 5 material table (the shipped one now has
/// half the dB; these tests are about the walk, not the values).
std::vector<TransmissionMaterial> materials() {
    auto m = [](MaterialKind kind, std::array<float, 3> crossing, std::array<float, 3> bulk) {
        TransmissionMaterial t;
        t.kind = kind;
        std::copy(crossing.begin(), crossing.end(), t.crossing_db);
        std::copy(bulk.begin(), bulk.end(), t.bulk_db_per_metre);
        return t;
    };
    return {
        m(MaterialKind::Air, {0, 0, 0}, {0, 0, 0}),
        m(MaterialKind::Solid, {25, 35, 45}, {12, 20, 30}),  // stone
        m(MaterialKind::Solid, {15, 22, 30}, {6, 10, 16}),   // wood
        m(MaterialKind::Solid, {12, 18, 24}, {4, 8, 12}),    // glass
        m(MaterialKind::Porous, {0.5f, 1, 2}, {1, 2, 4}),     // leaves
    };
}

/// A wall of `thickness` blocks of `material` across x = 10.., in chunk 0,0,0.
VoxelView wall(uint16_t material, int thickness) {
    auto c = std::make_shared<ChunkVoxels>();
    for (int x = 10; x < 10 + thickness; ++x) {
        for (int y = 0; y < kChunkSize; ++y) {
            for (int z = 0; z < kChunkSize; ++z) {
                c->materials[static_cast<std::size_t>(cell_index(x, y, z))] = material;
            }
        }
    }
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    return VoxelView(std::move(chunks), materials());
}

TransmissionTrace through(const VoxelView& view) {
    const double from[3] = {2.5, 16.5, 16.5};
    const double to[3] = {25.5, 16.5, 16.5};
    return view.trace(from, to);
}

}  // namespace

TEST_CASE("transmission: glass < wood < 1 stone < 3 stone < 6 stone, in every band") {
    const TransmissionTrace glass = through(wall(Glass, 1));
    const TransmissionTrace wood = through(wall(Wood, 1));
    const TransmissionTrace stone1 = through(wall(Stone, 1));
    const TransmissionTrace stone3 = through(wall(Stone, 3));
    const TransmissionTrace stone6 = through(wall(Stone, 6));
    for (int b = 0; b < 3; ++b) {
        CAPTURE(b);
        CHECK(glass.loss_db[b] < wood.loss_db[b]);
        CHECK(wood.loss_db[b] < stone1.loss_db[b]);
        CHECK(stone1.loss_db[b] < stone3.loss_db[b]);
        CHECK(stone3.loss_db[b] < stone6.loss_db[b]);
    }
    // Exactly: one crossing, plus the bulk loss over the thickness.
    CHECK(stone1.crossings == 1);
    CHECK(stone3.crossings == 1);
    CHECK(static_cast<double>(stone3.solid_metres) == doctest::Approx(3.0));
    CHECK(static_cast<double>(stone3.loss_db[1]) == doctest::Approx(35.0 + 3 * 20.0));
    CHECK(static_cast<double>(stone6.loss_db[2]) == doctest::Approx(45.0 + 6 * 30.0));
    // Higher bands lose more.
    CHECK(stone1.loss_db[0] < stone1.loss_db[2]);
    // Clear air loses nothing.
    const TransmissionTrace clear = through(wall(Air, 1));
    CHECK(!clear.blocked());
    CHECK(static_cast<double>(clear.gain(1)) == doctest::Approx(1.0));
}

TEST_CASE("transmission: a slanted path counts its true length, and materials in a row each cost a crossing") {
    auto c = std::make_shared<ChunkVoxels>();
    for (int y = 0; y < kChunkSize; ++y) {
        for (int z = 0; z < kChunkSize; ++z) {
            c->materials[static_cast<std::size_t>(cell_index(10, y, z))] = Stone;
            c->materials[static_cast<std::size_t>(cell_index(11, y, z))] = Wood;
        }
    }
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    // 45 degrees in x/z through 2 blocks: sqrt(2) metres in each.
    const double from[3] = {5.0, 16.5, 5.0};
    const double to[3] = {17.0, 16.5, 17.0};
    const TransmissionTrace t = view.trace(from, to);
    CHECK(t.crossings == 2);
    CHECK(static_cast<double>(t.solid_metres) == doctest::Approx(2.0 * std::sqrt(2.0)).epsilon(1e-6));
    CHECK(static_cast<double>(t.loss_db[1]) == doctest::Approx(35 + 22 + std::sqrt(2.0) * (20 + 10)).epsilon(1e-5));

    // A wall of leaves ten deep barely matters at low frequencies.
    const TransmissionTrace foliage = through(wall(Leaves, 10));
    CHECK(foliage.loss_db[0] < 12.0f);
    CHECK(foliage.crossings == 1);
}

TEST_CASE("transmission: partial blocks count the boxes the path passes, and unloaded chunks are air") {
    auto c = std::make_shared<ChunkVoxels>();
    PartialBlock door;
    door.cell = static_cast<uint16_t>(cell_index(10, 16, 16));
    door.material = Wood;
    door.boxes.push_back({{0.0f, 0.0f, 0.875f}, {1.0f, 1.0f, 1.0f}});  // a closed door: 1/8 thick, along x
    c->partials.push_back(door);
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());

    // Along z through the door leaf: 0.125 m of wood, one crossing.
    const double from[3] = {10.5, 16.5, 12.0};
    const double to[3] = {10.5, 16.5, 20.0};
    const TransmissionTrace through_door = view.trace(from, to);
    CHECK(through_door.crossings == 1);
    CHECK(static_cast<double>(through_door.solid_metres) == doctest::Approx(0.125));
    CHECK(static_cast<double>(through_door.loss_db[0]) == doctest::Approx(15 + 0.125 * 6));

    // Past its edge (the door is open to this path): nothing.
    const double beside[3] = {10.5, 16.5, 16.2};
    const double beyond[3] = {14.0, 16.5, 16.2};
    CHECK(!view.trace(beside, beyond).blocked());

    // Beyond the loaded chunk: air.
    const double far_from[3] = {40.0, 16.5, 16.5};
    const double far_to[3] = {80.0, 16.5, 16.5};
    CHECK(!view.trace(far_from, far_to).blocked());
}

TEST_CASE("transmission: a sound at the centre of a block escapes it towards the listener") {
    const VoxelView view = wall(Stone, 3);  // x 10..13
    double source[3] = {11.5, 16.5, 16.5};  // inside the wall
    const double listener[3] = {2.5, 16.5, 16.5};
    CHECK(view.escape(source, listener, 1));
    CHECK(source[0] < 11.0);  // moved one cell towards the listener, still in the wall
    CHECK(source[0] > 10.0);
    CHECK(view.escape(source, listener, 2));
    CHECK(source[0] < 10.0);  // out
    CHECK(!view.trace(source, listener).blocked());
    CHECK(!view.escape(source, listener, 2));  // already out

    // A block deep inside rock stays inside after the allowed steps.
    const VoxelView thick = wall(Stone, 6);
    double buried[3] = {14.5, 16.5, 16.5};
    thick.escape(buried, listener, 2);
    CHECK(thick.trace(buried, listener).blocked());
}

TEST_CASE("transmission: grazing the corner of a block costs little; a real passage costs in full") {
    auto c = std::make_shared<ChunkVoxels>();
    c->materials[static_cast<std::size_t>(cell_index(10, 16, 10))] = Stone;  // one block
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    // Through its corner: about 4 cm inside it.
    const double from[3] = {5.0, 16.5, 5.03};
    const double to[3] = {15.0, 16.5, 15.03};
    const double graze_from[3] = {10.95 - 5.0, 16.5, 10.0 - 5.0 - 0.02};
    const double graze_to[3] = {10.95 + 5.0, 16.5, 10.0 + 5.0 - 0.02};
    const TransmissionTrace graze = view.trace(graze_from, graze_to);
    MESSAGE("grazing: " << graze.solid_metres << " m, " << graze.loss_db[1] << " dB");
    CHECK(graze.solid_metres < 0.1f);
    CHECK(graze.loss_db[1] < 10.0f);
    const TransmissionTrace through = view.trace(from, to);
    CHECK(through.loss_db[1] > 35.0f);
}

TEST_CASE("transmission: clearance keeps a point off neighbouring solid faces") {
    auto c = std::make_shared<ChunkVoxels>();
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            c->materials[static_cast<std::size_t>(cell_index(x, 10, z))] = Stone;  // floor, top at 11
        }
    }
    c->materials[static_cast<std::size_t>(cell_index(6, 11, 5))] = Stone;  // a block beside
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    double feet[3] = {5.9, 11.0, 5.5};
    view.clearance(feet, 0.3);
    CHECK(feet[1] == doctest::Approx(11.3));
    CHECK(feet[0] == doctest::Approx(5.7));  // off the block at x 6
    CHECK(feet[2] == doctest::Approx(5.5));
    double inside[3] = {3.5, 10.5, 3.5};  // in the floor: left alone
    view.clearance(inside, 0.3);
    CHECK(inside[1] == doctest::Approx(10.5));
}

TEST_CASE("transmission: a sound inside a partial block (an anvil) leaves it, and clears its face") {
    auto c = std::make_shared<ChunkVoxels>();
    PartialBlock anvil;
    anvil.cell = static_cast<uint16_t>(cell_index(10, 16, 16));
    anvil.material = Wood;
    anvil.boxes.push_back({{0.1f, 0.0f, 0.25f}, {0.9f, 0.7f, 0.75f}});
    c->partials.push_back(anvil);
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    double source[3] = {10.5, 16.5, 16.5};  // the block's centre, inside its box
    const double listener[3] = {4.5, 16.5, 16.5};
    CHECK(view.trace(listener, source).crossings == 1);  // from inside, its own box counts
    CHECK(view.escape(source, listener, 2, 0.25));
    CHECK(source[0] == doctest::Approx(10.0 - 0.25).epsilon(0.01));
    CHECK(!view.trace(source, listener).blocked());
    // Air with nothing in it: no move.
    double open[3] = {6.5, 16.5, 16.5};
    CHECK(!view.escape(open, listener, 2, 0.25));
}

TEST_CASE("transmission: a listener beside an open door's leaf stays; one inside the leaf leaves the leaf, not the cell") {
    auto c = std::make_shared<ChunkVoxels>();
    PartialBlock door;
    door.cell = static_cast<uint16_t>(cell_index(10, 16, 16));
    door.material = Wood;
    door.boxes.push_back({{0.0f, 0.0f, 0.0f}, {0.125f, 1.0f, 1.0f}});  // open: the leaf along the cell's west side
    c->partials.push_back(door);
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    const double ahead[3] = {10.5, 16.5, 10.0};
    // Walking through the doorway: the cell is open where the listener is.
    double walking[3] = {10.5, 16.6, 16.7};
    CHECK(!view.escape(walking, ahead, 2, 1e-3, VoxelView::Escaping::Enclosures));
    CHECK(walking[2] == 16.7);
    // A camera pressed into the leaf comes out of the leaf's face, still in the doorway.
    double in_leaf[3] = {10.06, 16.6, 16.7};
    const double east[3] = {14.0, 16.6, 16.7};
    CHECK(view.escape(in_leaf, east, 2, 0.25, VoxelView::Escaping::Enclosures));
    CHECK(in_leaf[0] > 10.125);
    CHECK(in_leaf[0] < 10.5);
    CHECK(in_leaf[2] == 16.7);
}

TEST_CASE("transmission: a door's own sound leaves the door's cell past its leaf, towards the listener") {
    auto c = std::make_shared<ChunkVoxels>();
    PartialBlock door;
    door.cell = static_cast<uint16_t>(cell_index(10, 16, 16));
    door.material = Wood;
    door.boxes.push_back({{0.0f, 0.0f, 0.875f}, {1.0f, 1.0f, 1.0f}});  // closed: the leaf on the cell's south face
    c->partials.push_back(door);
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView view(std::move(chunks), materials());
    // The sound is placed at the block's centre, not inside the leaf; the listener stands south
    // of the door, the leaf between. The sound comes out past the leaf.
    double sound[3] = {10.5, 16.5, 16.5};
    const double outside[3] = {10.5, 16.6, 20.0};
    CHECK(view.trace(outside, sound).crossings == 1);
    CHECK(view.escape(sound, outside, 2, 0.25));
    CHECK(sound[2] > 17.0);
    CHECK(sound[2] < 17.5);
    CHECK(view.trace(outside, sound).crossings == 0);
    // The listener does not get that treatment: standing in the same cell, they stay.
    double standing[3] = {10.5, 16.6, 16.4};
    CHECK(!view.escape(standing, outside, 2, 1e-3, VoxelView::Escaping::Enclosures));
}

TEST_CASE("first hit: a wall's face, its outward normal, and a partial block's box") {
    const VoxelView view = wall(Stone, 2);  // x 10..12
    VoxelHit hit;
    const double from[3] = {5.0, 16.5, 16.5};
    const double east[3] = {1.0, 0.0, 0.0};
    REQUIRE(view.first_hit(from, east, 100.0, hit));
    CHECK(hit.distance == doctest::Approx(5.0));
    CHECK(hit.point[0] == doctest::Approx(10.0));
    CHECK(hit.normal[0] == -1.0);
    CHECK(hit.normal[1] == 0.0);
    CHECK(hit.material == Stone);
    CHECK_FALSE(hit.partial);
    // Out of reach, and away from it.
    CHECK_FALSE(view.first_hit(from, east, 4.0, hit));
    const double west[3] = {-1.0, 0.0, 0.0};
    CHECK_FALSE(view.first_hit(from, west, 100.0, hit));
    // Starting inside the wall: the cell it starts in is ignored, the next one is not.
    const double inside[3] = {10.5, 16.5, 16.5};
    REQUIRE(view.first_hit(inside, east, 100.0, hit));
    CHECK(hit.point[0] == doctest::Approx(11.0));
    const double beyond[3] = {11.5, 16.5, 16.5};
    CHECK_FALSE(view.first_hit(beyond, east, 100.0, hit));

    // A slab (lower half of the block at 20, 16, 16) is hit on its top face from above.
    auto c = std::make_shared<ChunkVoxels>();
    PartialBlock slab;
    slab.cell = static_cast<uint16_t>(cell_index(20, 16, 16));
    slab.material = Wood;
    slab.boxes.push_back(Box{{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}});
    c->partials.push_back(slab);
    VoxelView::Chunks chunks;
    chunks[{0, 0, 0}] = c;
    const VoxelView partial(std::move(chunks), materials());
    const double above[3] = {20.5, 20.0, 16.5};
    const double down[3] = {0.0, -1.0, 0.0};
    REQUIRE(partial.first_hit(above, down, 100.0, hit));
    CHECK(hit.point[1] == doctest::Approx(16.5));
    CHECK(hit.normal[1] == 1.0);
    CHECK(hit.material == Wood);
    CHECK(hit.partial);
}
