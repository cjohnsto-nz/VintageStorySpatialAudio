// The thread registry (Phase 8 profiling): registered threads report their CPU time by name and
// leave the sample when they end; a thread announced by id is found too.

#include "core/thread_stats.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace vsa;

namespace {

/// Spends about `ms` of CPU time.
void burn(double ms) {
    const auto start = std::chrono::steady_clock::now();
    volatile double sink = 0.0;
    while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < ms) {
        for (int i = 0; i < 1000; ++i) {
            sink = sink + std::sqrt(static_cast<double>(i));
        }
    }
}

const ThreadSample* find(const std::vector<ThreadSample>& samples, const char* name) {
    const auto it = std::find_if(samples.begin(), samples.end(), [&](const ThreadSample& s) { return s.name == name; });
    return it == samples.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("thread stats: registered threads report their CPU time by name, and leave when they end") {
    ThreadRegistry& registry = ThreadRegistry::instance();
    std::atomic<int> phase{0};
    std::thread worker([&] {
        ThreadScope scope("test worker");
        burn(60.0);
        phase.store(1);
        while (phase.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (phase.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        const std::vector<ThreadSample> samples = registry.sample();
        const ThreadSample* s = find(samples, "test worker");
        REQUIRE(s != nullptr);
        CHECK(s->kind == ThreadKind::Engine);
        CHECK(s->id != 0);
        CHECK(s->cpu_ms > 30.0);  // of the 60 ms burnt (scheduling and clock granularity aside)
        CHECK(s->cpu_ms < 2000.0);
    }
    phase.store(2);
    worker.join();
    CHECK(find(registry.sample(), "test worker") == nullptr);
}

TEST_CASE("thread stats: a thread announced by id is measured from another thread") {
    ThreadRegistry& registry = ThreadRegistry::instance();
    std::atomic<uint32_t> id{0};
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        id.store(ThreadRegistry::current_id());
        burn(40.0);
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (id.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    registry.announce(id.load(), "announced");
    registry.announce(id.load(), "announced");  // idempotent
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const std::vector<ThreadSample> samples = registry.sample();
    CHECK(std::count_if(samples.begin(), samples.end(), [](const ThreadSample& s) { return s.name == "announced"; }) == 1);
    const ThreadSample* s = find(samples, "announced");
    REQUIRE(s != nullptr);
    CHECK(s->id == id.load());
#if defined(_WIN32) || defined(__linux__)
    CHECK(s->cpu_ms > 20.0);
#endif
    stop.store(true);
    worker.join();
    // Gone with the thread: the announcement is not removed, but its handle measures nothing more.
}

TEST_CASE("thread stats: the calling thread's own time grows as it works") {
    ThreadRegistry& registry = ThreadRegistry::instance();
    registry.register_current("test main");
    const std::vector<ThreadSample> first = registry.sample();
    const ThreadSample* before = find(first, "test main");
    REQUIRE(before != nullptr);
    const double start = before->cpu_ms;
    burn(50.0);
    const std::vector<ThreadSample> after = registry.sample();
    const ThreadSample* s = find(after, "test main");
    REQUIRE(s != nullptr);
    CHECK(s->cpu_ms - start > 25.0);
    registry.unregister_current();
    CHECK(find(registry.sample(), "test main") == nullptr);
}
