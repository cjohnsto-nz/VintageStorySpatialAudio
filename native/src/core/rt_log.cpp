#include "core/rt_log.hpp"

#include "core/log.hpp"

namespace vsa {

void RtLog::drain() noexcept {
    Record record{};
    while (ring_.try_pop(record)) {
        Log::writef(record.level, "[render] %s (%lld, %lld)", record.message, static_cast<long long>(record.a),
                    static_cast<long long>(record.b));
    }
    const uint64_t dropped = dropped_.load(std::memory_order_relaxed);
    if (dropped != reported_dropped_) {
        Log::writef(VSA_LOG_WARNING, "[render] %llu log records dropped",
                    static_cast<unsigned long long>(dropped - reported_dropped_));
        reported_dropped_ = dropped;
    }
}

}  // namespace vsa
