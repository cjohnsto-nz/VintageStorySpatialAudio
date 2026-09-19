#include "core/spsc_ring.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <thread>

TEST_CASE("SpscRing rounds capacity up to a power of two and keeps FIFO order") {
    vsa::SpscRing<int> ring(5);
    CHECK(ring.capacity() == 8);
    CHECK(ring.free_space() == 8);

    for (int i = 0; i < 8; ++i) {
        CHECK(ring.try_push(i));
    }
    CHECK_FALSE(ring.try_push(99));
    CHECK(ring.free_space() == 0);
    CHECK(ring.size() == 8);

    int value = -1;
    for (int i = 0; i < 8; ++i) {
        REQUIRE(ring.try_pop(value));
        CHECK(value == i);
    }
    CHECK_FALSE(ring.try_pop(value));
    CHECK(ring.free_space() == 8);
}

TEST_CASE("SpscRing transfers a long sequence between two threads without loss or reordering") {
    constexpr uint64_t kCount = 2'000'000;
    vsa::SpscRing<uint64_t> ring(1024);

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount;) {
            if (ring.try_push(i)) {
                ++i;
            }
        }
    });

    uint64_t expected = 0;
    bool in_order = true;
    while (expected < kCount) {
        uint64_t value = 0;
        if (ring.try_pop(value)) {
            in_order = in_order && value == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK(ring.size() == 0);
}
