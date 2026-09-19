// Pathing through the public API (Phase 7, ADR 0014): the box round the listener is baked and
// baked again as the listener moves or blocks change; a sound behind a wall is heard the way
// round, from the doorway (the plan's goat-in-a-room acceptance test); a sealed room has no path.

#include "engine_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace vsa_test;

namespace {

enum : uint16_t { Air = 0, Stone = 1 };
constexpr uint32_t kRate = 48000;

vsa_engine_config pathing_config() {
    vsa_engine_config config = make_config(VSA_RAY_TRACER_AUTO);
    config.flags = VSA_ENGINE_FLAG_NO_REFLECTIONS;  // pathing (and the direct simulation) on
    config.pathing_range = 32;
    config.pathing_height = 16;
    return config;
}

void set_materials(OfflineEngine& e) {
    vsa_acoustic_material table[2]{};
    for (vsa_acoustic_material& m : table) {
        m.struct_size = sizeof m;
    }
    table[0].kind = VSA_MATERIAL_AIR;
    table[0].name = "air";
    table[1].kind = VSA_MATERIAL_SOLID;
    table[1].name = "stone";
    table[1].absorption[0] = table[1].absorption[1] = table[1].absorption[2] = 0.1f;
    table[1].scattering = 0.4f;
    // Almost nothing through the wall itself: what is heard comes round.
    table[1].transmission[0] = table[1].transmission[1] = table[1].transmission[2] = 0.001f;
    table[1].attenuation_db_per_metre[0] = table[1].attenuation_db_per_metre[1] = table[1].attenuation_db_per_metre[2] = 30.0f;
    REQUIRE(vsa_scene_set_materials(e.engine, table, 2) == VSA_OK);
}

std::size_t cell(int x, int y, int z) { return static_cast<std::size_t>((y * 32 + z) * 32 + x); }

/// A stone room, interior x 4..11, y 2..5, z 4..11, on a ground at y 1, with (unless sealed) a
/// two-block doorway in its south wall (z = 12) at x 7..8.
std::vector<uint16_t> room(bool doorway) {
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
    for (int z = 0; z < 32; ++z) {
        for (int x = 0; x < 32; ++x) {
            cells[cell(x, 1, z)] = Stone;
        }
    }
    for (int y = 2; y <= 6; ++y) {
        for (int z = 3; z <= 12; ++z) {
            for (int x = 3; x <= 12; ++x) {
                const bool shell = x == 3 || x == 12 || z == 3 || z == 12 || y == 6;
                const bool door = doorway && z == 12 && (x == 7 || x == 8) && (y == 2 || y == 3);
                if (shell && !door) {
                    cells[cell(x, y, z)] = Stone;
                }
            }
        }
    }
    return cells;
}

void set_chunk(OfflineEngine& e, const std::vector<uint16_t>& cells) {
    vsa_chunk_desc desc{};
    desc.struct_size = sizeof desc;
    desc.materials = cells.data();
    REQUIRE(vsa_scene_set_chunk(e.engine, &desc) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
}

vsa_pathing_stats stats(OfflineEngine& e) {
    vsa_pathing_stats s{};
    s.struct_size = sizeof s;
    REQUIRE(vsa_engine_get_pathing_stats(e.engine, &s) == VSA_OK);
    return s;
}

/// Renders `frames` of 7.1.4 output.
std::vector<float> render12(OfflineEngine& e, std::size_t frames) {
    std::vector<float> out(frames * 12);
    REQUIRE(vsa_engine_render_offline(e.engine, out.data(), static_cast<uint32_t>(frames)) == VSA_OK);
    return out;
}

/// Where the energy of a 7.1.4 render comes from: the energy-weighted mean of the ear-level
/// speakers' directions (listener space; +x right, -z ahead), and the total energy.
struct Lean {
    double x;
    double z;
    double energy;
};

Lean lean(const std::vector<float>& out) {
    // Engine order: FL FR FC LFE BL BR SL SR TFL TFR TBL TBR; azimuths clockwise from ahead.
    const double azimuth[12] = {-30, 30, 0, 0, -150, 150, -90, 90, -45, 45, -135, 135};
    double sx = 0.0;
    double sz = 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < out.size(); i += 12) {
        for (int c = 0; c < 12; ++c) {
            if (c == 3) {
                continue;
            }
            const auto sample = static_cast<double>(out[i + static_cast<std::size_t>(c)]);
            const double e = sample * sample;
            const double a = azimuth[c] * std::numbers::pi / 180.0;
            sx += e * std::sin(a);
            sz -= e * std::cos(a);
            total += e;
        }
    }
    return {total > 0.0 ? sx / total : 0.0, total > 0.0 ? sz / total : 0.0, total};
}

}  // namespace

TEST_CASE("pathing: the box round the listener is baked, and baked again when the listener moves on") {
    OfflineEngine e(pathing_config());
    set_materials(e);
    set_chunk(e, room(true));
    e.listener(8.0f, 3.6f, 16.0f, 0.0f, 0.0f, -1.0f);
    e.render(kRate / 4);
    vsa_pathing_stats s = stats(e);
    CHECK(s.enabled == 1);
    CHECK(s.bakes == 1);
    CHECK(s.probes > 10);
    CHECK(s.baking == 0);
    MESSAGE("first bake: " << s.probes << " probes in " << s.last_bake_ms << " ms; box centre " << s.box_centre[0] << "," << s.box_centre[1] << "," << s.box_centre[2]);
    // Within the middle third of the 32-block box: no new bake.
    e.listener(11.0f, 3.6f, 16.0f, 0.0f, 0.0f, -1.0f);
    e.render(kRate / 4);
    CHECK(stats(e).bakes == 1);
    // Beyond it: a new box round the listener.
    e.listener(24.0f, 3.6f, 16.0f, 0.0f, 0.0f, -1.0f);
    e.render(kRate / 4);
    s = stats(e);
    CHECK(s.bakes == 2);
    CHECK(s.box_centre[0] == doctest::Approx(24.0));
    // Blocks changed in the box: baked again after the quiet spell, not before.
    std::vector<uint16_t> cells = room(true);
    cells[cell(20, 2, 20)] = Stone;
    set_chunk(e, cells);
    e.render(kRate);
    CHECK(stats(e).bakes == 2);
    CHECK(stats(e).bake_due == 1);
    e.render(static_cast<std::size_t>(kRate) * 3);
    CHECK(stats(e).bakes == 3);
    CHECK(stats(e).bake_due == 0);
}

TEST_CASE("pathing: a sound in a room is heard through its doorway, from the doorway (goat and doorway)") {
    // The listener stands south of the room's south wall, 4 m from the doorway, looking north at
    // it; the goat bleats inside, off to the west (ahead-left). Through the wall almost nothing
    // comes; round the doorway the sound arrives from ahead, where the doorway is.
    Lean with_paths{};
    Lean without{};
    for (const bool pathing : {true, false}) {
        vsa_engine_config config = pathing_config();
        if (!pathing) {
            config.flags |= VSA_ENGINE_FLAG_NO_PATHING;
        }
        OfflineEngine e(config);
        REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
        vsa_output_desc desc{};
        desc.struct_size = sizeof desc;
        desc.kind = VSA_OUTPUT_NONE;
        desc.channels = 12;
        REQUIRE(vsa_output_open(e.engine, &desc) == VSA_OK);
        set_materials(e);
        set_chunk(e, room(true));
        e.listener(8.0f, 3.6f, 16.0f, 0.0f, 0.0f, -1.0f);
        render12(e, kRate / 2);  // baked, simulated
        const AssetPtr tone = e.pcm(sine(500.0, 48000.0, 48000, 0.3f), 1, kRate);
        REQUIRE(vsa_voice_start(e.engine, e.positioned(tone, VSA_SPATIAL_WORLD, 5.0f, 3.0f, 6.0f)) == VSA_OK);
        render12(e, kRate / 2);  // the path found, the effect settled
        const Lean l = lean(render12(e, kRate));
        if (pathing) {
            const vsa_pathing_stats s = stats(e);
            MESSAGE("pathing: " << s.found << " of " << s.wanted << " blocked sounds have a path; simulation " << s.last_tick_ms << " ms");
            CHECK(s.wanted == 1);
            CHECK(s.found == 1);
            with_paths = l;
        } else {
            without = l;
        }
    }
    MESSAGE("energy comes from x " << with_paths.x << " z " << with_paths.z << " with paths (" << 10.0 * std::log10(with_paths.energy)
            << " dB), x " << without.x << " z " << without.z << " without (" << 10.0 * std::log10(without.energy) << " dB)");
    // Louder by the way round, and from ahead (the doorway), not from ahead-left (the goat).
    CHECK(with_paths.energy > 10.0 * without.energy);
    CHECK(with_paths.z < -0.5);
    CHECK(std::abs(with_paths.x) < 0.2);
    CHECK(without.x < -0.2);
}

TEST_CASE("pathing: a sealed room has no path, and a sound in the open needs none") {
    OfflineEngine e(pathing_config());
    set_materials(e);
    set_chunk(e, room(false));
    e.listener(8.0f, 3.6f, 16.0f, 0.0f, 0.0f, -1.0f);
    const AssetPtr tone = e.pcm(sine(500.0, 48000.0, 48000, 0.3f), 1, kRate);
    const vsa_voice inside = e.positioned(tone, VSA_SPATIAL_WORLD, 5.0f, 3.0f, 6.0f);
    REQUIRE(vsa_voice_start(e.engine, inside) == VSA_OK);
    e.render(kRate);
    vsa_pathing_stats s = stats(e);
    CHECK(s.wanted == 1);
    CHECK(s.found == 0);
    REQUIRE(vsa_voice_stop(e.engine, inside) == VSA_OK);
    REQUIRE(vsa_voice_start(e.engine, e.positioned(tone, VSA_SPATIAL_WORLD, 8.0f, 3.0f, 20.0f)) == VSA_OK);  // in the open
    e.render(kRate);
    s = stats(e);
    CHECK(s.wanted == 0);
    std::vector<vsa_path_segment> segments(16);
    segments[0].struct_size = sizeof(vsa_path_segment);
    uint32_t count = 0;
    REQUIRE(vsa_engine_get_path_segments(e.engine, segments.data(), 16, &count) == VSA_OK);
    CHECK(count == 0);
}
