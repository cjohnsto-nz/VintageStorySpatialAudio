// The direct simulation through the public API (Phase 5): walls occlude and transmit what their
// material and thickness allow, per band; new sounds behind walls do not blip; moving past a
// corner is smooth; a sound inside a lone block escapes it; head-locked sounds are unaffected.

#include "engine_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>

using namespace vsa_test;

namespace {

enum : uint16_t { Air = 0, Stone = 1, Glass = 2 };

// Steam Audio's three bands: below 800 Hz, 800 Hz - 8 kHz, above 8 kHz.
constexpr double kBandTones[3] = {300.0, 2000.0, 12000.0};

float amplitude(float db) { return std::pow(10.0f, -db / 20.0f); }

void set_materials(OfflineEngine& e) {
    auto material = [](uint32_t kind, const char* name, float c0, float c1, float c2, float b0, float b1, float b2) {
        vsa_acoustic_material m{};
        m.struct_size = sizeof m;
        m.kind = kind;
        m.name = name;
        m.absorption[0] = m.absorption[1] = m.absorption[2] = 0.1f;
        m.transmission[0] = amplitude(c0);
        m.transmission[1] = amplitude(c1);
        m.transmission[2] = amplitude(c2);
        m.attenuation_db_per_metre[0] = b0;
        m.attenuation_db_per_metre[1] = b1;
        m.attenuation_db_per_metre[2] = b2;
        return m;
    };
    const vsa_acoustic_material table[] = {
        material(VSA_MATERIAL_AIR, "air", 0, 0, 0, 0, 0, 0),
        material(VSA_MATERIAL_SOLID, "stone", 25, 35, 45, 12, 20, 30),
        material(VSA_MATERIAL_SOLID, "glass", 12, 18, 24, 4, 8, 12),
    };
    REQUIRE(vsa_scene_set_materials(e.engine, table, 3) == VSA_OK);
}

std::size_t cell(int x, int y, int z) { return static_cast<std::size_t>((y * 32 + z) * 32 + x); }

/// A wall of `thickness` blocks from x = 10 across the chunk (y 0..31), for z < `z_end`.
void set_wall(OfflineEngine& e, uint16_t material, int thickness, int z_end = 32) {
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
    for (int x = 10; x < 10 + thickness; ++x) {
        for (int y = 0; y < 32; ++y) {
            for (int z = 0; z < z_end; ++z) {
                cells[cell(x, y, z)] = material;
            }
        }
    }
    vsa_chunk_desc d{};
    d.struct_size = sizeof d;
    d.materials = cells.data();
    REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
}

/// Three tones, one per band, at equal amplitude.
AssetPtr band_tones(OfflineEngine& e) {
    std::vector<float> x(4800, 0.0f);
    for (const double f : kBandTones) {
        const auto t = sine(f, 48000.0, 4800, 0.15f);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += t[i];
        }
    }
    return e.pcm(x, 1, 48000);
}

struct Bands {
    double db[3];
};

/// Per-band level (left channel), after settling.
Bands measure(OfflineEngine& e) {
    e.render(19200);  // 0.4 s: simulation primed and smoothing settled
    const auto out = e.render(24000);
    const auto left = channel(out, 2, 0);
    Bands b{};
    for (int k = 0; k < 3; ++k) {
        b.db[k] = to_db(fit_sine(left.data(), left.size(), kBandTones[k], 48000.0).amplitude);
    }
    return b;
}

/// Per-band level drop of a source behind `thickness` blocks of `material`, relative to no wall.
Bands drop(uint16_t material, int thickness) {
    const auto level = [&](bool wall) {
        OfflineEngine e;
        REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
        set_materials(e);
        set_wall(e, wall ? material : static_cast<uint16_t>(Air), thickness);
        e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);  // facing +x, towards the source
        const AssetPtr asset = band_tones(e);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 22.0f, 16.5f, 16.5f, 1.0f);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        return measure(e);
    };
    const Bands open = level(false);
    const Bands blocked = level(true);
    Bands d{};
    for (int k = 0; k < 3; ++k) {
        d.db[k] = open.db[k] - blocked.db[k];
    }
    return d;
}

}  // namespace

TEST_CASE("direct: a wall's loss per band follows its material and thickness") {
    const Bands glass = drop(Glass, 1);
    const Bands stone1 = drop(Stone, 1);
    const Bands stone3 = drop(Stone, 3);
    MESSAGE("glass " << glass.db[0] << " / " << glass.db[1] << " / " << glass.db[2] << " dB; stone " << stone1.db[0]
                     << " / " << stone1.db[1] << " / " << stone1.db[2] << " dB; 3 stone " << stone3.db[0] << " / "
                     << stone3.db[1] << " / " << stone3.db[2] << " dB");
    for (int b = 0; b < 3; ++b) {
        CAPTURE(b);
        CHECK(glass.db[b] > 6.0);
        CHECK(glass.db[b] < stone1.db[b]);
        CHECK(stone1.db[b] < stone3.db[b]);
    }
    // Fully occluded: what remains is the transmission (low band: 25 + 12 dB through one stone).
    CHECK(stone1.db[0] == doctest::Approx(37.0).epsilon(0.1));
    CHECK(glass.db[0] == doctest::Approx(16.0).epsilon(0.15));
    // Higher bands lose more.
    CHECK(stone1.db[0] < stone1.db[2]);
}

TEST_CASE("direct: the simulation reports what it sees for each source") {
    OfflineEngine e;
    set_materials(e);
    set_wall(e, Stone, 2);
    e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);
    const AssetPtr asset = band_tones(e);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 22.0f, 16.5f, 16.5f, 1.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(9600);

    vsa_source_debug sources[4] = {};
    sources[0].struct_size = sizeof(vsa_source_debug);
    uint32_t count = 0;
    REQUIRE(vsa_engine_get_sources(e.engine, sources, 4, &count) == VSA_OK);
    REQUIRE(count == 1);
    CHECK(sources[0].voice == v);
    CHECK(static_cast<double>(sources[0].occlusion) < 0.05);
    CHECK(sources[0].crossings == 1);
    CHECK(static_cast<double>(sources[0].solid_metres) == doctest::Approx(2.0));
    CHECK(static_cast<double>(sources[0].transmission[1]) == doctest::Approx(static_cast<double>(amplitude(35 + 2 * 20))).epsilon(0.01));
    CHECK((sources[0].flags & VSA_SOURCE_ESCAPED) == 0);

    vsa_simulation_stats stats{};
    stats.struct_size = sizeof stats;
    REQUIRE(vsa_engine_get_simulation_stats(e.engine, &stats) == VSA_OK);
    CHECK(stats.sources == 1);
    CHECK(stats.ticks > 3);
    CHECK(stats.rate_hz == 30);
    CHECK(stats.occlusion_samples == 16);
}

TEST_CASE("direct: a sound starting behind a wall does not blip at full level") {
    OfflineEngine e;
    REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
    set_materials(e);
    set_wall(e, Stone, 3);
    e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);
    e.render(4800);  // the simulation has run (with nothing to simulate)
    const AssetPtr asset = e.pcm(sine(300.0, 48000.0, 4800, 0.5f), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 22.0f, 16.5f, 16.5f, 1.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    const auto onset = e.render(4800);  // the first 100 ms
    // Unoccluded it would peak near 0.5 / 18 m (-31 dBFS); through 3 stone it is ~61 dB lower.
    CHECK(to_db(peak(onset)) < -70.0);
}

TEST_CASE("direct: moving out from behind a wall is smooth") {
    OfflineEngine e;
    REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
    set_materials(e);
    set_wall(e, Stone, 1, 16);  // the wall ends at z = 16
    e.listener(4.0f, 16.5f, 8.0f, 1.0f, 0.0f, 0.0f);
    const AssetPtr asset = e.pcm(sine(200.0, 48000.0, 4800, 0.5f), 1, 48000);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 14.0f, 16.5f, 8.0f, 1.0f);
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(9600);
    std::vector<float> channels[2];
    for (int step = 0; step <= 100; ++step) {
        // From behind the wall (z 8) to well past its end (z 28) over one second.
        REQUIRE(vsa_voice_set_position(e.engine, v, VSA_SPATIAL_WORLD, 14.0f, 16.5f, 8.0f + 0.2f * static_cast<float>(step)) ==
                VSA_OK);
        const auto out = e.render(480);
        for (uint32_t c = 0; c < 2; ++c) {
            const auto x = channel(out, 2, c);
            channels[c].insert(channels[c].end(), x.begin(), x.end());
        }
    }
    double start = 0.0;
    double open = 0.0;
    for (const auto& x : channels) {
        start = std::max(start, peak(std::vector<float>(x.begin(), x.begin() + 4800)));
        open = std::max(open, peak(std::vector<float>(x.end() - 4800, x.end())));
        // No step larger than the tone's own (an unsmoothed gain change would be a click).
        const double natural = 2.0 * 3.14159265 * 200.0 / 48000.0 * peak(x);
        CHECK(max_step(x.data(), x.size()) < natural * 1.5);
    }
    // Behind the wall: ~37 dB through the stone, less ~7 dB for being closer than at the end.
    MESSAGE("behind the wall " << to_db(start) << " dB, past its end " << to_db(open) << " dB");
    CHECK(to_db(start) < to_db(open) - 20.0);
}

TEST_CASE("direct: a sound inside a lone block is not muffled by it; head-locked sounds ignore walls") {
    const auto level = [](bool block, uint32_t spatial) {
        OfflineEngine e;
        REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
        set_materials(e);
        std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
        if (block) {
            cells[cell(20, 16, 16)] = Stone;  // the source sits inside this block
            for (int y = 0; y < 32; ++y) {
                for (int z = 0; z < 32; ++z) {
                    cells[cell(10, y, z)] = spatial == VSA_SPATIAL_LISTENER ? Stone : Air;
                }
            }
        }
        vsa_chunk_desc d{};
        d.struct_size = sizeof d;
        d.materials = cells.data();
        REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
        REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
        e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);
        const AssetPtr asset = band_tones(e);
        // World: at the block's centre. Head-locked: 16 m ahead, beyond the wall at x 10.
        const vsa_voice v = spatial == VSA_SPATIAL_WORLD ? e.positioned(asset, VSA_SPATIAL_WORLD, 20.5f, 16.5f, 16.5f, 1.0f)
                                                         : e.positioned(asset, VSA_SPATIAL_LISTENER, 0.0f, 0.0f, -16.0f, 1.0f);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const Bands b = measure(e);
        uint32_t count = 0;
        vsa_source_debug s{};
        s.struct_size = sizeof s;
        REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &count) == VSA_OK);
        if (block && spatial == VSA_SPATIAL_WORLD) {
            CHECK(count == 1);
            CHECK((s.flags & VSA_SOURCE_ESCAPED) != 0);
            CHECK(static_cast<double>(s.simulated_position[0]) < 20.0);
        }
        if (spatial == VSA_SPATIAL_LISTENER) {
            CHECK(count == 0);  // not simulated
        }
        return b;
    };
    const Bands open = level(false, VSA_SPATIAL_WORLD);
    const Bands inside = level(true, VSA_SPATIAL_WORLD);
    const Bands head_open = level(false, VSA_SPATIAL_LISTENER);
    const Bands head_walled = level(true, VSA_SPATIAL_LISTENER);
    for (int b = 0; b < 3; ++b) {
        CAPTURE(b);
        MESSAGE("band " << b << ": open " << open.db[b] << ", inside a block " << inside.db[b] << " dB");
        CHECK(open.db[b] - inside.db[b] < 4.0);
        CHECK(std::abs(head_open.db[b] - head_walled.db[b]) < 0.1);
    }
}

TEST_CASE("direct: the in-game configuration (Embree, far-from-zero coordinates, device thread) sees walls") {
    for (const vsa_ray_tracer tracer : {VSA_RAY_TRACER_STEAM, VSA_RAY_TRACER_AUTO}) {
        for (const bool device : {false, true}) {
            CAPTURE(static_cast<int>(tracer));
            CAPTURE(device);
            OfflineEngine e(make_config(tracer));
            set_materials(e);
            std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
            for (int y = 0; y < 32; ++y) {
                for (int z = 0; z < 32; ++z) {
                    cells[cell(10, y, z)] = Stone;
                }
            }
            REQUIRE(vsa_scene_set_origin(e.engine, 512000, 64, 512000) == VSA_OK);
            vsa_chunk_desc d{};
            d.struct_size = sizeof d;
            d.x = 16000;
            d.y = 2;
            d.z = 16000;
            d.materials = cells.data();
            REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
            REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
            e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);
            const AssetPtr asset = band_tones(e);
            const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 20.0f, 16.5f, 16.5f, 1.0f);
            REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
            if (device) {
                vsa_output_desc out{};
                out.struct_size = sizeof out;
                out.kind = VSA_OUTPUT_DEVICE;
                if (vsa_output_open(e.engine, &out) != VSA_OK) {
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            } else {
                e.render(9600);
            }
            vsa_source_debug s{};
            s.struct_size = sizeof s;
            uint32_t n = 0;
            REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &n) == VSA_OK);
            REQUIRE(n == 1);
            CHECK(static_cast<double>(s.occlusion) < 0.05);
            CHECK(s.crossings == 1);
        }
    }
}

TEST_CASE("direct: the simulation listens from the eyes, not the rendering offset, and never from inside rock") {
    const auto simulate = [](float listener_x, float offset_x, float forward_x) {
        OfflineEngine e;
        set_materials(e);
        std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
        for (int y = 0; y < 32; ++y) {
            for (int z = 0; z < 32; ++z) {
                cells[cell(9, y, z)] = Stone;  // a wall just behind the listener (x 9..10)
            }
        }
        vsa_chunk_desc d{};
        d.struct_size = sizeof d;
        d.materials = cells.data();
        REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
        REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
        e.listener(listener_x, 16.5f, 16.5f, forward_x, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, offset_x, 0.0f, 0.0f);
        const AssetPtr asset = band_tones(e);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 20.0f, 16.5f, 16.5f, 1.0f);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        e.render(9600);
        vsa_source_debug s{};
        s.struct_size = sizeof s;
        uint32_t n = 0;
        REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &n) == VSA_OK);
        vsa_simulation_stats stats{};
        stats.struct_size = sizeof stats;
        REQUIRE(vsa_engine_get_simulation_stats(e.engine, &stats) == VSA_OK);
        return std::make_pair(s, stats);
    };
    // Back to the wall: the eyes at x 10.3, rendering offset 0.5 m back (inside the wall).
    const auto [clear, stats] = simulate(10.3f, -0.5f, 1.0f);
    CHECK(static_cast<double>(clear.occlusion) > 0.95);
    CHECK(clear.crossings == 0);
    CHECK(static_cast<double>(stats.listener[0]) == doctest::Approx(10.3));

    // A camera inside the wall (third person) listens from where it leaves the rock, looking +x.
    const auto [escaped, escaped_stats] = simulate(9.5f, 0.0f, 1.0f);
    CHECK(static_cast<double>(escaped.occlusion) > 0.95);
    CHECK(static_cast<double>(escaped_stats.listener[0]) > 10.0);
}

TEST_CASE("direct: a sound resting on the floor is not half hidden by it") {
    OfflineEngine e;
    set_materials(e);
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
    for (int z = 0; z < 32; ++z) {
        for (int x = 0; x < 32; ++x) {
            cells[cell(x, 10, z)] = Stone;  // a floor, top at y 11
        }
    }
    vsa_chunk_desc d{};
    d.struct_size = sizeof d;
    d.materials = cells.data();
    REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
    e.listener(4.0f, 12.6f, 16.5f, 1.0f, 0.0f, 0.0f);  // standing on the floor
    const AssetPtr asset = band_tones(e);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 7.0f, 11.0f, 16.5f, 1.0f);  // at someone's feet
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(9600);
    vsa_source_debug s{};
    s.struct_size = sizeof s;
    uint32_t n = 0;
    REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &n) == VSA_OK);
    MESSAGE("feet on the floor: occlusion " << s.occlusion << ", simulated at y " << s.simulated_position[1]);
    CHECK(static_cast<double>(s.occlusion) > 0.9);
    CHECK(s.crossings == 0);
    CHECK(static_cast<double>(s.simulated_position[1]) >= 11.4);
}

TEST_CASE("direct: occlusion survives chunks being rebuilt again and again, on both ray tracers") {
    // Opening a door rebuilds its chunk; Steam Audio's Embree scenes went blind after the second
    // such change (see test_embree_scene_edits.cpp).
    for (const vsa_ray_tracer tracer : {VSA_RAY_TRACER_STEAM, VSA_RAY_TRACER_AUTO}) {
        CAPTURE(static_cast<int>(tracer));
        OfflineEngine e(make_config(tracer));
        set_materials(e);
        std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
        for (int y = 0; y < 32; ++y) {
            for (int z = 0; z < 32; ++z) {
                cells[cell(10, y, z)] = Stone;
            }
        }
        vsa_chunk_desc d{};
        d.struct_size = sizeof d;
        d.materials = cells.data();
        REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
        REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
        e.listener(4.0f, 16.5f, 16.5f, 1.0f, 0.0f, 0.0f);
        const AssetPtr asset = band_tones(e);
        const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 20.0f, 16.5f, 16.5f, 1.0f);
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        std::vector<uint16_t> neighbour(VSA_CHUNK_CELLS, Stone);
        for (int round = 0; round < 10; ++round) {
            CAPTURE(round);
            cells[cell(20 + round, 5, 5)] = Stone;  // an edit elsewhere in the chunk (a door)
            REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
            if (round % 3 == 1) {  // other chunks coming and going too
                vsa_chunk_desc n{};
                n.struct_size = sizeof n;
                n.x = 3;
                n.materials = neighbour.data();
                REQUIRE(vsa_scene_set_chunk(e.engine, &n) == VSA_OK);
            } else if (round % 3 == 2) {
                REQUIRE(vsa_scene_remove_chunk(e.engine, 3, 0, 0) == VSA_OK);
            }
            REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
            e.render(4800);
            vsa_source_debug s{};
            s.struct_size = sizeof s;
            uint32_t n = 0;
            REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &n) == VSA_OK);
            CHECK(static_cast<double>(s.occlusion) < 0.05);
        }
    }
}

TEST_CASE("direct: a sound inside its own partial block (an anvil) is not hidden by it") {
    OfflineEngine e;
    set_materials(e);
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
    for (int z = 0; z < 32; ++z) {
        for (int x = 0; x < 32; ++x) {
            cells[cell(x, 15, z)] = Stone;  // the floor the anvil stands on
        }
    }
    const vsa_box box{{0.1f, 0.0f, 0.25f}, {0.9f, 0.7f, 0.75f}};
    const vsa_partial_block anvil{static_cast<uint32_t>(cell(10, 16, 16)), Stone, 0, 1};
    vsa_chunk_desc d{};
    d.struct_size = sizeof d;
    d.materials = cells.data();
    d.partials = &anvil;
    d.partial_count = 1;
    d.boxes = &box;
    d.box_count = 1;
    REQUIRE(vsa_scene_set_chunk(e.engine, &d) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
    e.listener(4.5f, 17.6f, 16.5f, 1.0f, 0.0f, 0.0f);
    const AssetPtr asset = band_tones(e);
    const vsa_voice v = e.positioned(asset, VSA_SPATIAL_WORLD, 10.5f, 16.5f, 16.5f, 1.0f);  // the block's centre
    REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
    e.render(9600);
    vsa_source_debug s{};
    s.struct_size = sizeof s;
    uint32_t n = 0;
    REQUIRE(vsa_engine_get_sources(e.engine, &s, 1, &n) == VSA_OK);
    MESSAGE("anvil: occlusion " << s.occlusion << ", crossings " << s.crossings << ", simulated at " << s.simulated_position[0]
                                << "," << s.simulated_position[1] << "," << s.simulated_position[2]);
    CHECK((s.flags & VSA_SOURCE_ESCAPED) != 0);
    CHECK(static_cast<double>(s.occlusion) > 0.9);
    CHECK(s.crossings == 0);
}
