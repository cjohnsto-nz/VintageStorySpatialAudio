#include "audio/channel_layout.hpp"

#include <doctest/doctest.h>

using vsa::map_to_device;
using vsa::Speaker;
using enum vsa::Speaker;

TEST_CASE("a device in the engine's own order maps one to one") {
    for (const uint32_t channels : {2u, 4u, 6u, 8u}) {
        CAPTURE(channels);
        const auto layout = vsa::steam_layout(channels);
        const auto map = map_to_device(channels, layout.data());
        for (uint32_t c = 0; c < channels; ++c) {
            CHECK(map[c] == static_cast<int>(c));
        }
    }
}

TEST_CASE("5.1 devices with side surrounds receive the rear channels") {
    const Speaker device[] = {FrontLeft, FrontRight, FrontCentre, Lfe, SideLeft, SideRight};
    const auto map = map_to_device(6, device);
    CHECK(map[4] == 4);  // engine BL -> device SL
    CHECK(map[5] == 5);
    CHECK(map[2] == 2);
    CHECK(map[3] == 3);
}

TEST_CASE("channels are routed by speaker, not by position") {
    // A 7.1 device ordering its surrounds sides-first.
    const Speaker device[] = {FrontLeft, FrontRight, FrontCentre, Lfe, SideLeft, SideRight, BackLeft, BackRight};
    const auto map = map_to_device(8, device);
    CHECK(map[4] == 6);  // engine BL
    CHECK(map[5] == 7);  // engine BR
    CHECK(map[6] == 4);  // engine SL
    CHECK(map[7] == 5);  // engine SR
}

TEST_CASE("speakers the device lacks are dropped, never doubled up") {
    const Speaker device[] = {FrontLeft, FrontRight, Other, Other};
    const auto map = map_to_device(4, device);
    CHECK(map[0] == 0);
    CHECK(map[1] == 1);
    CHECK(map[2] == -1);
    CHECK(map[3] == -1);
}
