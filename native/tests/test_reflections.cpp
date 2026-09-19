// Reflections through the public API (Phase 6): the simulated decay time tracks the size of a
// room or cave (the phase's golden test), the rendered reverb decays at it, long sounds get
// reflections of their own and short ones share the listener's, slots are handed on cleanly, and
// the debugging views report what happens.

#include "engine_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace vsa_test;

namespace {

enum : uint16_t { Air = 0, Stone = 1 };

constexpr uint32_t kRate = 48000;

vsa_engine_config reflection_config(uint32_t sources = 0) {
    vsa_engine_config config = make_config(VSA_RAY_TRACER_AUTO);
    config.flags = VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION;  // reflections on
    config.reflection_rays = 2048;
    config.reflection_bounces = 32;
    config.reflection_sources = sources;
    return config;
}

/// Plaster-like walls: 20 % absorbed in every band.
void set_materials(OfflineEngine& e, float absorption = 0.2f) {
    vsa_acoustic_material table[2]{};
    for (vsa_acoustic_material& m : table) {
        m.struct_size = sizeof m;
    }
    table[0].kind = VSA_MATERIAL_AIR;
    table[0].name = "air";
    table[1].kind = VSA_MATERIAL_SOLID;
    table[1].name = "stone";
    table[1].absorption[0] = table[1].absorption[1] = table[1].absorption[2] = absorption;
    table[1].scattering = 0.3f;
    REQUIRE(vsa_scene_set_materials(e.engine, table, 2) == VSA_OK);
}

std::size_t cell(int x, int y, int z) { return static_cast<std::size_t>((y * 32 + z) * 32 + x); }

struct Room {
    int w, h, d;  // interior, blocks
    /// The interior's centre (scene = world coordinates here), at ear height.
    [[nodiscard]] float cx() const { return 2.0f + static_cast<float>(w) / 2.0f; }
    [[nodiscard]] float cy() const { return 2.0f + std::min(1.7f, static_cast<float>(h) / 2.0f); }
    [[nodiscard]] float cz() const { return 2.0f + static_cast<float>(d) / 2.0f; }
    /// Eyring's decay time for walls absorbing `a`.
    [[nodiscard]] double eyring(double a) const {
        const double v = static_cast<double>(w) * h * d;
        const double s = 2.0 * (static_cast<double>(w) * h + static_cast<double>(w) * d + static_cast<double>(h) * d);
        return 0.161 * v / (-s * std::log(1.0 - a));
    }
};

/// A closed stone box around the interior [2, 2 + size) on each axis (one chunk); or, for an
/// open field, only a floor at y = 1.
void build(OfflineEngine& e, const Room* room) {
    std::vector<uint16_t> cells(VSA_CHUNK_CELLS, Air);
    if (room == nullptr) {
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 32; ++x) {
                cells[cell(x, 1, z)] = Stone;
            }
        }
    } else {
        for (int y = 1; y <= 2 + room->h; ++y) {
            for (int z = 1; z <= 2 + room->d; ++z) {
                for (int x = 1; x <= 2 + room->w; ++x) {
                    const bool inside = x >= 2 && x < 2 + room->w && y >= 2 && y < 2 + room->h && z >= 2 &&
                                        z < 2 + room->d;
                    if (!inside) {
                        cells[cell(x, y, z)] = Stone;
                    }
                }
            }
        }
    }
    vsa_chunk_desc desc{};
    desc.struct_size = sizeof desc;
    desc.materials = cells.data();
    REQUIRE(vsa_scene_set_chunk(e.engine, &desc) == VSA_OK);
    REQUIRE(vsa_scene_wait_idle(e.engine, 10000) == VSA_OK);
}

vsa_reflection_stats stats(OfflineEngine& e) {
    vsa_reflection_stats s{};
    s.struct_size = sizeof s;
    REQUIRE(vsa_engine_get_reflection_stats(e.engine, &s) == VSA_OK);
    return s;
}

std::vector<vsa_reflection_source> sources(OfflineEngine& e) {
    std::vector<vsa_reflection_source> out(64);
    out[0].struct_size = sizeof(vsa_reflection_source);
    uint32_t count = 0;
    REQUIRE(vsa_engine_get_reflection_sources(e.engine, out.data(), static_cast<uint32_t>(out.size()), &count) == VSA_OK);
    out.resize(std::min<std::size_t>(count, out.size()));
    return out;
}

/// A one-shot voice at a position.
vsa_voice one_shot(OfflineEngine& e, const AssetPtr& asset, float x, float y, float z, float gain = 1.0f) {
    vsa_voice_desc desc{};
    desc.struct_size = sizeof desc;
    desc.asset = asset.get();
    desc.bus = VSA_BUS_SOUND;
    desc.gain = gain;
    desc.pitch = 1.0f;
    desc.spatial = VSA_SPATIAL_WORLD;
    desc.position[0] = x;
    desc.position[1] = y;
    desc.position[2] = z;
    desc.min_distance = 1.0f;
    vsa_voice v = 0;
    REQUIRE(vsa_voice_create(e.engine, &desc, &v) == VSA_OK);
    return v;
}

/// A Hann-windowed 2 kHz burst (Steam Audio's middle band), `seconds` long.
std::vector<float> burst(double seconds, double frequency = 2000.0) {
    const auto n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
        x[i] = static_cast<float>(0.5 * w * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate));
    }
    return x;
}

/// Energy per frame of an interleaved stereo render.
std::vector<double> frame_energy(const std::vector<float>& stereo) {
    std::vector<double> e(stereo.size() / 2);
    for (std::size_t i = 0; i < e.size(); ++i) {
        const auto l = static_cast<double>(stereo[2 * i]);
        const auto r = static_cast<double>(stereo[2 * i + 1]);
        e[i] = l * l + r * r;
    }
    return e;
}

/// RT60 from the Schroeder decay curve of `e` from `start`: three times the time from -5 to -25 dB.
double schroeder_rt60(const std::vector<double>& e, std::size_t start) {
    std::vector<double> edc(e.size() - start);
    double sum = 0.0;
    for (std::size_t i = e.size(); i > start; --i) {
        sum += e[i - 1];
        edc[i - 1 - start] = sum;
    }
    const double total = edc[0];
    std::size_t t5 = 0;
    std::size_t t25 = 0;
    for (std::size_t i = 0; i < edc.size(); ++i) {
        const double db = 10.0 * std::log10(edc[i] / total);
        if (t5 == 0 && db <= -5.0) {
            t5 = i;
        }
        if (db <= -25.0) {
            t25 = i;
            break;
        }
    }
    return t25 > t5 ? 3.0 * static_cast<double>(t25 - t5) / kRate : 0.0;
}

}  // namespace

TEST_CASE("reflections: the simulated decay time tracks the size of a room or cave (golden)") {
    const Room rooms[] = {{4, 3, 4}, {12, 6, 12}, {26, 14, 26}};
    const char* names[] = {"small room", "large room", "cave"};
    double previous = 0.0;
    for (int i = 0; i < 3; ++i) {
        CAPTURE(names[i]);
        OfflineEngine e(reflection_config());
        set_materials(e);
        build(e, &rooms[i]);
        e.listener(rooms[i].cx(), rooms[i].cy(), rooms[i].cz(), 0.0f, 0.0f, -1.0f);
        e.render(kRate);  // ten simulations, accumulating
        const vsa_reflection_stats s = stats(e);
        REQUIRE(s.enabled == 1);
        CHECK(s.ticks >= 9);
        const auto rt = static_cast<double>(s.listener_reverb_times[1]);
        const double expected = rooms[i].eyring(0.2);
        MESSAGE(std::string(names[i]) << ": RT60 " << s.listener_reverb_times[0] << " / " << rt << " / " << s.listener_reverb_times[2]
                         << " s (Eyring " << expected << " s); a simulation takes " << s.last_tick_ms << " ms");
        CHECK(rt > 1.3 * previous);
        CHECK(rt > 0.6 * expected);
        CHECK(rt < 1.6 * expected);
        previous = rt;
    }
}

TEST_CASE("reflections: an open field hardly reverberates") {
    const Room small{4, 3, 4};
    double room_db = 0.0;
    double field_db = 0.0;
    for (const bool open : {false, true}) {
        OfflineEngine e(reflection_config());
        set_materials(e);
        build(e, open ? nullptr : &small);
        e.listener(small.cx(), small.cy(), small.cz(), 0.0f, 0.0f, -1.0f);
        e.render(kRate / 2);
        const AssetPtr noise = e.pcm(burst(1.0), 1, kRate);
        const vsa_voice v = e.positioned(noise, VSA_SPATIAL_WORLD, small.cx() + 1.0f, small.cy(), small.cz());
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        e.render(kRate);
        (open ? field_db : room_db) = static_cast<double>(stats(e).output_db);
    }
    MESSAGE("reflections: " << room_db << " dB in a small room, " << field_db << " dB in an open field");
    CHECK(field_db < room_db - 10.0);
}

TEST_CASE("reflections: the rendered reverb decays at the simulated decay time") {
    const Room rooms[] = {{4, 3, 4}, {26, 14, 26}};
    double measured[2] = {};
    for (int i = 0; i < 2; ++i) {
        const Room& room = rooms[i];
        OfflineEngine e(reflection_config());
        REQUIRE(vsa_engine_set_render_mode(e.engine, VSA_RENDER_SPEAKERS) == VSA_OK);
        set_materials(e);
        build(e, &room);
        e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
        e.render(kRate / 2);  // the listener's reverb is simulated
        const AssetPtr click = e.pcm(burst(0.02), 1, kRate);
        const vsa_voice v = one_shot(e, click, room.cx() + 1.5f, room.cy(), room.cz());
        REQUIRE(vsa_voice_start(e.engine, v) == VSA_OK);
        const std::vector<float> out = e.render(static_cast<std::size_t>(kRate) * 3);
        const std::vector<double> energy = frame_energy(out);
        // After the burst and its direct sound (and the limiter's look-ahead).
        const double rt = schroeder_rt60(energy, static_cast<std::size_t>(0.05 * kRate));
        const auto simulated = static_cast<double>(stats(e).listener_reverb_times[1]);
        MESSAGE("room " << room.w << "x" << room.h << "x" << room.d << ": rendered RT60 " << rt << " s, simulated " << simulated
                        << " s");
        CHECK(rt == doctest::Approx(simulated).epsilon(0.2));
        measured[i] = rt;
    }
    CHECK(measured[1] > 2.0 * measured[0]);
}

TEST_CASE("reflections: nothing is added when they are off") {
    const Room room{12, 6, 12};
    double late[2] = {};
    for (const bool on : {true, false}) {
        vsa_engine_config config = reflection_config();
        if (!on) {
            config.flags |= VSA_ENGINE_FLAG_NO_REFLECTIONS;
        }
        OfflineEngine e(config);
        set_materials(e);
        build(e, &room);
        e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
        e.render(kRate / 2);
        const AssetPtr click = e.pcm(burst(0.02), 1, kRate);
        REQUIRE(vsa_voice_start(e.engine, one_shot(e, click, room.cx() + 1.5f, room.cy(), room.cz())) == VSA_OK);
        const std::vector<double> energy = frame_energy(e.render(kRate));
        double sum = 0.0;
        for (std::size_t i = kRate / 10; i < energy.size(); ++i) {
            sum += energy[i];
        }
        late[on ? 0 : 1] = sum;
        if (!on) {
            CHECK(stats(e).enabled == 0);
        }
    }
    CHECK(late[1] == 0.0);  // the burst is over by 100 ms: without reflections, silence
    CHECK(late[0] > 1e-4);
}

TEST_CASE("reflections: long sounds get their own; short ones share a spot simulated where they happen") {
    const Room room{12, 6, 12};
    OfflineEngine e(reflection_config());
    set_materials(e);
    build(e, &room);
    e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
    const AssetPtr tone = e.pcm(burst(1.0), 1, kRate);
    const AssetPtr blip = e.pcm(burst(0.2), 1, kRate);
    const vsa_voice looping = e.positioned(tone, VSA_SPATIAL_WORLD, room.cx() + 3.0f, room.cy(), room.cz());
    REQUIRE(vsa_voice_start(e.engine, looping) == VSA_OK);
    REQUIRE(vsa_voice_start(e.engine, one_shot(e, blip, room.cx() - 4.0f, room.cy(), room.cz())) == VSA_OK);
    e.render(kRate / 2);
    CHECK(stats(e).live_slots == 2);  // the looping voice's own, and the short one's spot
    std::vector<vsa_reflection_source> list = sources(e);
    REQUIRE(list.size() == 3);
    CHECK(list[0].slot == 0);
    CHECK(list[0].voice == 0);
    const auto own = std::find_if(list.begin(), list.end(), [&](const vsa_reflection_source& s) { return s.voice == looping; });
    REQUIRE(own != list.end());
    CHECK(static_cast<double>(own->position[0]) == doctest::Approx(static_cast<double>(room.cx() + 3.0f)));
    CHECK(own->reverb_times[1] > 0.3f);
    const auto spot = std::find_if(list.begin(), list.end(), [&](const vsa_reflection_source& s) {
        return s.slot > 0 && s.voice != looping;
    });
    REQUIRE(spot != list.end());
    CHECK(static_cast<double>(spot->position[0]) == doctest::Approx(static_cast<double>(room.cx() - 4.0f)));

    // More short sounds nearby share that spot; none makes another.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(vsa_voice_start(e.engine, one_shot(e, blip, room.cx() - 4.0f + static_cast<float>(i), room.cy(),
                                                   room.cz() + 1.0f)) == VSA_OK);
        e.render(kRate / 4);
    }
    CHECK(stats(e).live_slots == 2);
    CHECK(sources(e).size() == 3);

    // Once nothing has sounded there for a while, the spot is let go.
    e.render(static_cast<std::size_t>(kRate) * 22);
    CHECK(stats(e).live_slots == 1);
}

TEST_CASE("reflections: a sound behind walls reverberates only as much as reaches the listener") {
    // The listener in a closed stone room; a short sound inside it, or outside it.
    // Its first play has no spot yet and excites the listener's reverb: by what arrives, walls
    // included. (Its spot outside, simulated by Steam Audio, finds no way in either.)
    const Room room{8, 4, 8};
    double peak_db[2] = {};
    for (const bool outside : {false, true}) {
        vsa_engine_config config = reflection_config();
        config.flags = 0;  // the direct simulation too: occlusion and transmission
        OfflineEngine e(config);
        set_materials(e);
        build(e, &room);
        e.listener(room.cx() - 2.0f, room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
        e.render(kRate / 2);
        const AssetPtr blip = e.pcm(burst(0.5), 1, kRate);
        // 4 m away inside the room, or 9 m away beyond its east wall (x 10..11).
        const float x = outside ? room.cx() - 2.0f + 9.0f : room.cx() + 2.0f;
        REQUIRE(vsa_voice_start(e.engine, one_shot(e, blip, x, room.cy(), room.cz())) == VSA_OK);
        double peak = -200.0;
        for (int i = 0; i < 10; ++i) {
            e.render(kRate / 10);
            peak = std::max(peak, static_cast<double>(stats(e).output_db));
        }
        peak_db[outside ? 1 : 0] = peak;
    }
    MESSAGE("listener's reverb from a sound inside the room " << peak_db[0] << " dB, from one outside its stone walls "
                                                              << peak_db[1] << " dB");
    CHECK(peak_db[1] < peak_db[0] - 30.0);
}

TEST_CASE("reflections: a sound's reverb scales with its direct sound, whatever its reference distance") {
    // Game sounds keep full level within a reference distance (3 m and more): the same sound at
    // 3 m with a reference of 1 m is 9.5 dB quieter than with 8 m, direct and reflected alike.
    const Room room{12, 6, 12};
    double level_db[2] = {};
    for (const float reference : {1.0f, 8.0f}) {
        OfflineEngine e(reflection_config());
        set_materials(e);
        build(e, &room);
        e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
        const AssetPtr tone = e.pcm(burst(1.0), 1, kRate);
        REQUIRE(vsa_voice_start(e.engine, e.positioned(tone, VSA_SPATIAL_WORLD, room.cx() + 3.0f, room.cy(), room.cz(),
                                                        reference)) == VSA_OK);
        e.render(static_cast<std::size_t>(kRate) * 2);
        REQUIRE(stats(e).live_slots == 1);
        level_db[reference > 1.0f ? 1 : 0] = static_cast<double>(stats(e).output_db);
    }
    MESSAGE("reflections " << level_db[1] - level_db[0] << " dB louder with an 8 m reference (direct: 9.5 dB)");
    CHECK(level_db[1] - level_db[0] == doctest::Approx(9.54).epsilon(0.15));
}

TEST_CASE("reflections: slots are handed on as voices come and go") {
    const Room room{12, 6, 12};
    OfflineEngine e(reflection_config(4));
    set_materials(e);
    build(e, &room);
    e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
    const AssetPtr tone = e.pcm(burst(1.0), 1, kRate);
    std::vector<vsa_voice> voices;
    for (int i = 0; i < 10; ++i) {
        const double angle = 2.0 * std::numbers::pi * i / 10.0;
        voices.push_back(e.positioned(tone, VSA_SPATIAL_WORLD, room.cx() + 4.0f * static_cast<float>(std::cos(angle)),
                                      room.cy(), room.cz() + 4.0f * static_cast<float>(std::sin(angle)),
                                      1.0f, 0.1f + 0.08f * static_cast<float>(i)));
        REQUIRE(vsa_voice_start(e.engine, voices.back()) == VSA_OK);
    }
    e.render(kRate);
    vsa_reflection_stats s = stats(e);
    CHECK(s.slots == 4);
    CHECK(s.live_slots == 4);
    // The four loudest (the last four) have them.
    std::vector<vsa_reflection_source> list = sources(e);
    for (const vsa_reflection_source& source : list) {
        if (source.slot > 0) {
            CHECK(std::find(voices.end() - 4, voices.end(), source.voice) != voices.end());
        }
    }
    // Stop the loudest four: their slots drain and go to the next four.
    for (std::size_t i = 6; i < 10; ++i) {
        REQUIRE(vsa_voice_stop(e.engine, voices[i]) == VSA_OK);
    }
    std::vector<float> out = e.render(static_cast<std::size_t>(kRate) * 4);
    for (const float x : out) {
        REQUIRE(std::isfinite(x));
    }
    s = stats(e);
    CHECK(s.live_slots == 4);
    list = sources(e);
    for (const vsa_reflection_source& source : list) {
        if (source.slot > 0) {
            CHECK(std::find(voices.begin() + 2, voices.begin() + 6, source.voice) != voices.begin() + 6);
        }
    }
    // Everything stops: every slot drains and is free again.
    for (const vsa_voice v : voices) {
        REQUIRE(vsa_voice_stop(e.engine, v) == VSA_OK);
    }
    e.render(static_cast<std::size_t>(kRate) * 5);
    s = stats(e);
    CHECK(s.live_slots == 0);
    CHECK(s.waiting_slots == 0);
    CHECK(s.draining_slots == 0);
}

TEST_CASE("reflections: the gain scales them") {
    const Room room{4, 3, 4};
    OfflineEngine e(reflection_config());
    set_materials(e);
    build(e, &room);
    e.listener(room.cx(), room.cy(), room.cz(), 0.0f, 0.0f, -1.0f);
    const AssetPtr tone = e.pcm(burst(1.0), 1, kRate);
    REQUIRE(vsa_voice_start(e.engine, e.positioned(tone, VSA_SPATIAL_WORLD, room.cx() + 1.0f, room.cy(), room.cz())) == VSA_OK);
    e.render(kRate);
    const float on_db = stats(e).output_db;
    REQUIRE(vsa_engine_set_reflection_gain(e.engine, 0.5f) == VSA_OK);
    e.render(kRate);
    const float half_db = stats(e).output_db;
    CHECK(stats(e).gain == 0.5f);
    CHECK(std::abs(half_db - (on_db - 6.02f)) < 1.0f);
    REQUIRE(vsa_engine_set_reflection_gain(e.engine, 0.0f) == VSA_OK);
    e.render(kRate);
    CHECK(stats(e).output_db < on_db - 12.0f);  // the meter falls with a 0.3 s time constant
    CHECK(vsa_engine_set_reflection_gain(e.engine, 5.0f) == VSA_ERROR_INVALID_ARGUMENT);
    CHECK(vsa_engine_set_reflection_gain(e.engine, NAN) == VSA_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("reflections: settings out of range are refused") {
    for (const auto& change : {+[](vsa_engine_config& c) { c.reflection_order = 4; },
                               +[](vsa_engine_config& c) { c.reflection_rays = 100; },
                               +[](vsa_engine_config& c) { c.reflection_duration = 0.1f; },
                               +[](vsa_engine_config& c) { c.reflection_transition = 1.0f; },
                               +[](vsa_engine_config& c) { c.reflection_sources = 65; }}) {
        vsa_engine_config config = reflection_config();
        change(config);
        ScopedEngine bad(config);
        CHECK(bad.result == VSA_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("trace rays: paths stay in a closed room, and leave an open field") {
    const Room room{6, 4, 6};
    for (const bool open : {false, true}) {
        CAPTURE(open);
        OfflineEngine e;
        set_materials(e);
        build(e, open ? nullptr : &room);
        const float origin[3] = {room.cx(), room.cy(), room.cz()};
        std::vector<vsa_ray_segment> segments(64 * 9);
        segments[0].struct_size = sizeof(vsa_ray_segment);
        uint32_t count = 0;
        REQUIRE(vsa_scene_trace_rays(e.engine, origin, 64, 8, 50.0f, segments.data(),
                                     static_cast<uint32_t>(segments.size()), &count) == VSA_OK);
        segments.resize(count);
        const auto escaped = std::count_if(segments.begin(), segments.end(),
                                           [](const vsa_ray_segment& s) { return s.material == 0; });
        if (open) {
            CHECK(escaped == 64);  // every ray leaves, up at once or after bouncing off the floor
            CHECK(count < 64 * 3);
        } else {
            CHECK(escaped == 0);
            CHECK(count == 64 * 9);
            for (const vsa_ray_segment& s : segments) {
                CHECK(s.energy == doctest::Approx(std::pow(0.8, s.bounce + 1)).epsilon(0.001));
                for (int k = 0; k < 3; ++k) {
                    CHECK(s.to[k] >= 1.99f);  // on or inside the walls' inner faces
                }
            }
        }
    }
}

