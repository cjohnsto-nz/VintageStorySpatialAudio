#pragma once

#include "core/spsc_ring.hpp"
#include "vsaudio.h"

#include <atomic>
#include <cstdint>

namespace vsa {

/// Log records posted by the real-time render thread. The message must be a string literal (only
/// the pointer is stored); the worker thread formats and forwards records to Log. Posting never
/// allocates, locks or blocks; when the queue is full the record is dropped and counted.
class RtLog {
public:
    struct Record {
        vsa_log_level level;
        const char* message;
        int64_t a;
        int64_t b;
    };

    RtLog() : ring_(256) {}

    void post(vsa_log_level level, const char* literal, int64_t a = 0, int64_t b = 0) noexcept {
        if (!ring_.try_push(Record{level, literal, a, b})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// Worker side: forwards every queued record to Log.
    void drain() noexcept;

private:
    SpscRing<Record> ring_;
    std::atomic<uint64_t> dropped_{0};
    uint64_t reported_dropped_ = 0;
};

}  // namespace vsa
