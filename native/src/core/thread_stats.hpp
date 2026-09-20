#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vsa {

enum class ThreadKind : uint32_t {
    Engine = 0,      // one of ours, by name
    SteamAudio = 1,  // Steam Audio's own workers (its ray tracing runs on threads it creates)
    Other = 2,       // the process's: the game, the runtime, drivers; named by module
};

struct ThreadSample {
    std::string name;
    ThreadKind kind = ThreadKind::Other;
    uint32_t id = 0;
    /// User + kernel time since the thread started, in milliseconds.
    double cpu_ms = 0.0;
};

/// Names the engine's threads and measures their CPU time (Phase 8: what the mod costs against
/// vanilla, by thread). Threads the engine owns register themselves as they start
/// (`ThreadScope`); the device's callback thread, which must not allocate, publishes its id and
/// the engine worker announces it. `sample()` reports every registered thread; on Windows it
/// also walks the rest of the process and names each thread by the module its start address
/// lies in, so Steam Audio's workers (phonon) and the game's own threads (coreclr, OpenAL, the
/// display driver) can be told apart from ours. Process-wide, like threads are.
class ThreadRegistry {
public:
    static ThreadRegistry& instance();

    /// Registers the calling thread under `name` (renaming it if already registered).
    void register_current(const std::string& name);
    void unregister_current();
    /// Names a thread by its id from another thread; idempotent for the same id and name.
    void announce(uint32_t id, const std::string& name);
    /// Every registered thread that still runs, then (Windows) the process's other threads.
    [[nodiscard]] std::vector<ThreadSample> sample() const;

    [[nodiscard]] static uint32_t current_id() noexcept;

    ThreadRegistry(const ThreadRegistry&) = delete;
    ThreadRegistry& operator=(const ThreadRegistry&) = delete;

private:
    struct Entry {
        std::string name;
        uint32_t id = 0;
        void* handle = nullptr;  // Windows: a handle with query access
        long clock = -1;         // POSIX: the thread's CPU clock id (registered threads only)
    };

    ThreadRegistry() = default;
    ~ThreadRegistry();
    static void close(Entry& entry) noexcept;
    static double cpu_ms(const Entry& entry) noexcept;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

/// Registers the calling thread for its lifetime: the first line of a thread's main function.
class ThreadScope {
public:
    explicit ThreadScope(const char* name) { ThreadRegistry::instance().register_current(name); }
    ~ThreadScope() { ThreadRegistry::instance().unregister_current(); }
    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;
};

}  // namespace vsa
