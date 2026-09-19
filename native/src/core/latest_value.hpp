#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace vsa {

/// Single-writer / single-reader mailbox that always yields the most recently published value
/// (a triple buffer). Neither side blocks or allocates; intermediate values may be skipped.
template <typename T>
class LatestValue {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    explicit LatestValue(const T& initial = T{}) noexcept {
        for (T& slot : slots_) {
            slot = initial;
        }
    }

    /// Writer side.
    void publish(const T& value) noexcept {
        slots_[back_] = value;
        // Hand the written slot over and take the previously shared one; flag it as fresh.
        const uint32_t old = shared_.exchange(back_ | kFresh, std::memory_order_acq_rel);
        back_ = old & kIndexMask;
    }

    /// Reader side: the latest published value (the previous one if nothing new was published).
    const T& read() noexcept {
        if ((shared_.load(std::memory_order_relaxed) & kFresh) != 0) {
            const uint32_t old = shared_.exchange(front_, std::memory_order_acq_rel);
            front_ = old & kIndexMask;
        }
        return slots_[front_];
    }

private:
    static constexpr uint32_t kFresh = 4;
    static constexpr uint32_t kIndexMask = 3;

    T slots_[3];
    std::atomic<uint32_t> shared_{1};
    uint32_t back_ = 0;   // writer-owned
    uint32_t front_ = 2;  // reader-owned
};

}  // namespace vsa
