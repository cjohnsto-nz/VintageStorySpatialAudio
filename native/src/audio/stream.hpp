#pragma once

#include "decode/decoders.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace vsa {

class Asset;

/// Decode-ahead state of one voice playing a streamed asset.
///
/// The worker thread decodes into a single-producer/single-consumer ring of interleaved floats
/// (about one second); the render thread pulls from it into a planar window that the resampler
/// reads. Looping is done by the worker (the ring simply continues from the start), so the
/// render thread sees one endless sequence of "virtual" frames counted from the last seek point.
///
/// Seeks are requested by the render thread with a generation number. The worker repositions the
/// decoder, notes the ring index where the new generation's data begins and publishes the
/// generation; the render thread then skips its read index to that point (discarding stale data)
/// and outputs silence until then. Only the consumer ever moves the read index and only the
/// producer the write index, so no data race is possible.
class Stream {
public:
    enum class Status { Ready, Waiting, Starved };

    static constexpr int64_t kUnknownEnd = std::numeric_limits<int64_t>::max();

    /// `history_frames`: frames of left context kept before the read position (resampler taps);
    /// `window_frames`: largest span the render thread will ever request. Allocates; throws.
    Stream(const Asset& asset, uint32_t history_frames, uint32_t window_frames);

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // ---- worker side (one thread at a time; the engine serialises with its stream lock) ----

    /// Handles a pending seek and tops the ring up. Never blocks on the render thread.
    void service() noexcept;

    // ---- render side ----

    /// Restarts reading at asset frame `frame`; virtual frame 0 becomes that frame.
    void request_seek(uint64_t frame) noexcept;
    void set_looping(bool looping) noexcept { looping_.store(looping, std::memory_order_release); }

    /// Makes the window cover virtual frames [first, end). Past the end of a finished stream the
    /// window holds zeros. Waiting: a seek is still in flight. Starved: the ring ran dry.
    Status prepare_window(int64_t first, int64_t end) noexcept;
    [[nodiscard]] const float* const* window() const noexcept { return window_ptrs_; }
    [[nodiscard]] int64_t window_first() const noexcept { return window_first_; }

    /// Virtual frame at which the stream ends; kUnknownEnd while not yet known or looping.
    [[nodiscard]] int64_t end_virtual() const noexcept { return end_virtual_; }
    /// Asset frame corresponding to virtual frame 0.
    [[nodiscard]] uint64_t base_frame() const noexcept { return base_frame_; }

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint64_t memory_bytes() const noexcept;

private:
    void reset_window() noexcept;

    const Asset& asset_;
    std::unique_ptr<decode::VorbisReader> reader_;
    uint32_t channels_;
    uint32_t history_;

    // Ring: interleaved, `ring_frames_` frames, monotonic indices.
    std::vector<float> ring_;
    uint64_t ring_frames_;
    std::atomic<uint64_t> write_frame_{0};  // producer (worker)
    std::atomic<uint64_t> read_frame_{0};   // consumer (render)

    // Render -> worker.
    std::atomic<uint32_t> seek_request_gen_{0};
    std::atomic<uint64_t> seek_request_frame_{0};
    std::atomic<bool> looping_{false};

    // Worker -> render.
    std::atomic<uint32_t> produced_gen_{0};
    std::atomic<uint64_t> gen_start_frame_{0};
    std::atomic<uint64_t> end_frame_{std::numeric_limits<uint64_t>::max()};

    // Worker only.
    uint32_t worker_gen_ = 0;
    uint64_t worker_gen_start_ = 0;
    bool eof_ = false;
    bool failed_ = false;

    // Render only.
    uint32_t render_gen_ = 0;
    bool awaiting_ = false;
    uint64_t gen_start_ = 0;
    uint64_t base_frame_ = 0;
    int64_t end_virtual_ = kUnknownEnd;
    std::vector<float> window_storage_;
    float* window_ptrs_[decode::kMaxChannels] = {};
    uint32_t window_capacity_ = 0;
    int64_t window_first_ = 0;
    uint32_t window_len_ = 0;
};

}  // namespace vsa
