// The world scene through the C ABI: materials, chunks, stats, mesh read-back, validation.

#include "engine_fixture.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace vsa_test;

namespace {

std::vector<vsa_acoustic_material> materials() {
    vsa_acoustic_material air{};
    air.struct_size = sizeof air;
    air.kind = VSA_MATERIAL_AIR;
    air.name = "air";
    vsa_acoustic_material stone{};
    stone.struct_size = sizeof stone;
    stone.kind = VSA_MATERIAL_SOLID;
    stone.absorption[0] = stone.absorption[1] = stone.absorption[2] = 0.05f;
    stone.name = "stone";
    return {air, stone};
}

std::size_t cell(int x, int y, int z) { return static_cast<std::size_t>((y * 32 + z) * 32 + x); }

vsa_chunk_desc chunk_desc(const std::vector<uint16_t>& cells, int32_t x = 0, int32_t y = 0, int32_t z = 0) {
    vsa_chunk_desc d{};
    d.struct_size = sizeof d;
    d.x = x;
    d.y = y;
    d.z = z;
    d.materials = cells.data();
    return d;
}

}  // namespace

TEST_CASE("scene ABI: a chunk becomes a mesh that can be read back and listed") {
    OfflineEngine e;
    const auto table = materials();
    REQUIRE(vsa_scene_set_materials(e.engine, table.data(), static_cast<uint32_t>(table.size())) == VSA_OK);

    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, 0);
    cells[cell(4, 4, 4)] = 1;
    // A slab next to it, as a partial block.
    const vsa_box box{{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}};
    const vsa_partial_block slab{static_cast<uint32_t>(cell(10, 4, 4)), 1, 0, 1};
    vsa_chunk_desc desc = chunk_desc(cells, 100, 3, -7);
    desc.partials = &slab;
    desc.partial_count = 1;
    desc.boxes = &box;
    desc.box_count = 1;
    REQUIRE(vsa_scene_set_chunk(e.engine, &desc) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);

    vsa_scene_stats stats{};
    stats.struct_size = sizeof stats;
    REQUIRE(vsa_scene_get_stats(e.engine, &stats) == VSA_OK);
    CHECK(stats.chunks == 1);
    CHECK(stats.meshed_chunks == 1);
    CHECK(stats.triangles == 24);  // two boxes' worth (the cube's faces all face in-chunk air)
    CHECK(stats.material_count == 2);
    CHECK(stats.pending_chunks == 0);

    vsa_chunk_mesh mesh{};
    mesh.struct_size = sizeof mesh;
    REQUIRE(vsa_scene_get_chunk_mesh(e.engine, 100, 3, -7, &mesh) == VSA_OK);  // query the sizes
    CHECK(mesh.found == 1);
    CHECK(mesh.triangle_count == 24);
    CHECK(mesh.version > 0);
    std::vector<float> vertices(mesh.vertex_count * 3);
    std::vector<int32_t> triangles(mesh.triangle_count * 3);
    std::vector<uint16_t> ids(mesh.triangle_count);
    mesh.vertex_capacity = mesh.vertex_count;
    mesh.triangle_capacity = mesh.triangle_count;
    mesh.vertices = vertices.data();
    mesh.triangles = triangles.data();
    mesh.materials = ids.data();
    REQUIRE(vsa_scene_get_chunk_mesh(e.engine, 100, 3, -7, &mesh) == VSA_OK);
    for (const uint16_t id : ids) {
        CHECK(id == 1);
    }
    float top = 0.0f;
    for (std::size_t v = 0; v < vertices.size(); v += 3) {
        top = std::max(top, vertices[v + 1]);
    }
    CHECK(static_cast<double>(top) == doctest::Approx(5.0));  // chunk-local: the cube's top at y 5

    int32_t keys[6] = {};
    uint32_t count = 0;
    REQUIRE(vsa_scene_list_chunks(e.engine, keys, 2, &count) == VSA_OK);
    CHECK(count == 1);
    CHECK(keys[0] == 100);
    CHECK(keys[1] == 3);
    CHECK(keys[2] == -7);

    mesh = vsa_chunk_mesh{};
    mesh.struct_size = sizeof mesh;
    REQUIRE(vsa_scene_get_chunk_mesh(e.engine, 0, 0, 0, &mesh) == VSA_OK);
    CHECK(mesh.found == 0);

    const std::string path = (std::filesystem::temp_directory_path() / "vsaudio-abi-scene.obj").string();
    REQUIRE(vsa_scene_save_obj(e.engine, path.c_str()) == VSA_OK);
    CHECK(std::filesystem::file_size(path) > 100);
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path).replace_extension(".mtl"));

    REQUIRE(vsa_scene_set_origin(e.engine, 3200, 96, -224) == VSA_OK);
    REQUIRE(vsa_scene_remove_chunk(e.engine, 100, 3, -7) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
    REQUIRE(vsa_scene_get_stats(e.engine, &stats) == VSA_OK);
    CHECK(stats.chunks == 0);
    CHECK(stats.triangles == 0);
    CHECK(stats.origin[0] == 3200);
    CHECK(stats.origin[2] == -224);
}

TEST_CASE("scene ABI: invalid materials and chunks are rejected") {
    OfflineEngine e;
    auto table = materials();
    table[0].kind = VSA_MATERIAL_SOLID;
    CHECK(vsa_scene_set_materials(e.engine, table.data(), 2) == VSA_ERROR_INVALID_ARGUMENT);  // id 0 must be air
    table[0].kind = VSA_MATERIAL_AIR;
    table[1].kind = 7;
    CHECK(vsa_scene_set_materials(e.engine, table.data(), 2) == VSA_ERROR_INVALID_ARGUMENT);
    table[1].kind = VSA_MATERIAL_SOLID;
    table[1].struct_size = 4;
    CHECK(vsa_scene_set_materials(e.engine, table.data(), 2) == VSA_ERROR_ABI_MISMATCH);
    table[1].struct_size = sizeof(vsa_acoustic_material);
    REQUIRE(vsa_scene_set_materials(e.engine, table.data(), 2) == VSA_OK);

    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, 0);
    cells[5] = 2;  // no material 2
    vsa_chunk_desc desc = chunk_desc(cells);
    CHECK(vsa_scene_set_chunk(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    cells[5] = 1;
    desc.lod = 2;
    CHECK(vsa_scene_set_chunk(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    desc.lod = 1;
    const vsa_partial_block bad{VSA_CHUNK_CELLS, 1, 0, 0};
    desc.partials = &bad;
    desc.partial_count = 1;
    CHECK(vsa_scene_set_chunk(e.engine, &desc) == VSA_ERROR_INVALID_ARGUMENT);
    desc.partials = nullptr;
    desc.partial_count = 0;
    CHECK(vsa_scene_set_chunk(e.engine, &desc) == VSA_OK);
    CHECK(vsa_scene_save_obj(e.engine, "") == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_scene_clear(e.engine) == VSA_OK);
    CHECK(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
}

TEST_CASE("scene ABI: a ray finds the surface, its material, and the block behind it") {
    OfflineEngine e;
    const auto table = materials();
    REQUIRE(vsa_scene_set_materials(e.engine, table.data(), static_cast<uint32_t>(table.size())) == VSA_OK);
    // Chunk 1000,2,-5 with the origin at its corner: a stone block at local 4,4,4 and a slab at 10,4,4.
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, 0);
    cells[cell(4, 4, 4)] = 1;
    const vsa_box box{{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 1.0f}};
    const vsa_partial_block slab{static_cast<uint32_t>(cell(10, 4, 4)), 1, 0, 1};
    vsa_chunk_desc desc = chunk_desc(cells, 1000, 2, -5);
    desc.partials = &slab;
    desc.partial_count = 1;
    desc.boxes = &box;
    desc.box_count = 1;
    REQUIRE(vsa_scene_set_origin(e.engine, 32000, 64, -160) == VSA_OK);
    REQUIRE(vsa_scene_set_chunk(e.engine, &desc) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);

    vsa_ray_hit hit{};
    hit.struct_size = sizeof hit;
    // From above the stone block, straight down: its top at scene y 5.
    const float down[3] = {0.0f, -1.0f, 0.0f};
    const float above_stone[3] = {4.5f, 10.0f, 4.5f};
    REQUIRE(vsa_scene_raycast(e.engine, above_stone, down, 50.0f, &hit) == VSA_OK);
    REQUIRE(hit.hit == 1);
    CHECK(static_cast<double>(hit.distance) == doctest::Approx(5.0));
    CHECK(static_cast<double>(hit.normal[1]) == doctest::Approx(1.0));
    CHECK(hit.material == 1);
    CHECK(hit.from_partial == 0);
    CHECK(hit.cell[0] == 32004);
    CHECK(hit.cell[1] == 68);
    CHECK(hit.cell[2] == -156);
    CHECK(hit.chunk[0] == 1000);
    CHECK(hit.lod == 0);

    // Onto the slab: its top at scene y 4.5, from a partial block.
    const float above_slab[3] = {10.5f, 10.0f, 4.5f};
    REQUIRE(vsa_scene_raycast(e.engine, above_slab, down, 50.0f, &hit) == VSA_OK);
    REQUIRE(hit.hit == 1);
    CHECK(static_cast<double>(hit.distance) == doctest::Approx(5.5));
    CHECK(hit.from_partial == 1);
    CHECK(hit.cell[0] == 32010);
    CHECK(hit.cell[1] == 68);

    // Sideways into the stone block's -x face, and a miss beyond the distance.
    const float west[3] = {1.0f, 0.0f, 0.0f};
    const float beside[3] = {0.5f, 4.5f, 4.5f};
    REQUIRE(vsa_scene_raycast(e.engine, beside, west, 50.0f, &hit) == VSA_OK);
    CHECK(static_cast<double>(hit.distance) == doctest::Approx(3.5));
    CHECK(static_cast<double>(hit.normal[0]) == doctest::Approx(-1.0));
    CHECK(hit.cell[0] == 32004);
    REQUIRE(vsa_scene_raycast(e.engine, beside, west, 3.0f, &hit) == VSA_OK);
    CHECK(hit.hit == 0);
    const float nowhere[3] = {0.0f, 0.0f, 0.0f};
    CHECK(vsa_scene_raycast(e.engine, beside, nowhere, 50.0f, &hit) == VSA_OK);
    CHECK(hit.hit == 0);
}
