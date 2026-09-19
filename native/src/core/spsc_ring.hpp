#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace vsa {

inline constexpr std::size_t kCacheLine = 64;

/// Bounded single-producer / single-consumer queue of trivially copyable items.
///
/// Storage is allocated once in the constructor; push and pop never allocate, lock or wait, so
/// either side may be the real-time thread. Several producers (or consumers) are allowed only if
/// they serialise themselves with their own lock. Indices are monotonic 64-bit counters.
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing items are copied with plain assignment");

public:
    /// Capacity is rounded up to a power of two.
    explicit SpscRing(std::size_t min_capacity) {
        std::size_t capacity = 1;
        while (capacity < min_capacity) {
            capacity <<= 1;
        }
        items_ = std::make_unique<T[]>(capacity);
        mask_ = capacity - 1;
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // ---- producer side ----

    /// Free slots as seen by the producer (exact from the producer's point of view: it can only grow).
    [[nodiscard]] std::size_t free_space() noexcept {
        const uint64_t write = write_.load(std::memory_order_relaxed);
        read_cache_ = read_.load(std::memory_order_acquire);
        return capacity() - static_cast<std::size_t>(write - read_cache_);
    }

    bool try_push(const T& item) noexcept {
        const uint64_t write = write_.load(std::memory_order_relaxed);
        if (write - read_cache_ >= capacity()) {
            read_cache_ = read_.load(std::memory_order_acquire);
            if (write - read_cache_ >= capacity()) {
                return false;
            }
        }
        items_[write & mask_] = item;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    // ---- consumer side ----

    bool try_pop(T& out) noexcept {
        const uint64_t read = read_.load(std::memory_order_relaxed);
        if (read == write_cache_) {
            write_cache_ = write_.load(std::memory_order_acquire);
            if (read == write_cache_) {
                return false;
            }
        }
        out = items_[read & mask_];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    /// Items currently queued. Approximate when called from neither side.
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(write_.load(std::memory_order_acquire) -
                                        read_.load(std::memory_order_acquire));
    }

private:
    std::unique_ptr<T[]> items_;
    std::size_t mask_ = 0;
    alignas(kCacheLine) std::atomic<uint64_t> write_{0};
    uint64_t read_cache_ = 0;  // producer-owned snapshot of read_
    alignas(kCacheLine) std::atomic<uint64_t> read_{0};
    uint64_t write_cache_ = 0;  // consumer-owned snapshot of write_
};

}  // namespace vsa
