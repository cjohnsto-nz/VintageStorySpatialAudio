#include "audio/stream.hpp"

#include "audio/asset.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace vsa {
namespace {
constexpr uint64_t kNoEnd = std::numeric_limits<uint64_t>::max();
constexpr int kDecodeChunk = 4096;
}  // namespace

Stream::Stream(const Asset& asset, uint32_t history_frames, uint32_t window_frames)
    : asset_(asset), channels_(asset.channels()), history_(history_frames) {
    reader_ = std::make_unique<decode::VorbisReader>(asset.encoded().data(), asset.encoded().size());
    ring_frames_ = std::max<uint64_t>(asset.sample_rate(), 16384);  // about one second
    ring_.resize(static_cast<std::size_t>(ring_frames_) * channels_);

    window_capacity_ = window_frames + history_frames;
    window_storage_.resize(static_cast<std::size_t>(window_capacity_) * channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
        window_ptrs_[c] = window_storage_.data() + static_cast<std::size_t>(c) * window_capacity_;
    }
    reset_window();
}

uint64_t Stream::memory_bytes() const noexcept {
    return (ring_.size() + window_storage_.size()) * sizeof(float);
}

void Stream::reset_window() noexcept {
    // History before virtual frame 0 is silence.
    window_first_ = -static_cast<int64_t>(history_);
    window_len_ = history_;
    for (uint32_t c = 0; c < channels_; ++c) {
        std::fill(window_ptrs_[c], window_ptrs_[c] + history_, 0.0f);
    }
}

// ---- worker side ----

void Stream::service() noexcept {
    if (failed_) {
        return;
    }
    const auto fail = [&](const char* what) {
        failed_ = true;
        eof_ = true;
        end_frame_.store(write_frame_.load(std::memory_order_relaxed), std::memory_order_release);
        Log::writef(VSA_LOG_ERROR, "stream '%s': %s; the voice will end early", asset_.name().c_str(), what);
    };

    const uint32_t requested = seek_request_gen_.load(std::memory_order_acquire);
    if (requested != worker_gen_) {
        const uint64_t frame = seek_request_frame_.load(std::memory_order_relaxed);
        worker_gen_ = requested;
        eof_ = false;
        worker_gen_start_ = write_frame_.load(std::memory_order_relaxed);
        end_frame_.store(kNoEnd, std::memory_order_relaxed);
        gen_start_frame_.store(worker_gen_start_, std::memory_order_relaxed);
        produced_gen_.store(requested, std::memory_order_release);
        if (!reader_->seek(frame)) {
            fail("seek failed");
            return;
        }
    }

    const bool looping = looping_.load(std::memory_order_acquire);
    if (eof_ && looping) {
        // Looping was switched on after the end was reached: carry on from the start.
        if (!reader_->seek(0)) {
            fail("seek failed");
            return;
        }
        eof_ = false;
        end_frame_.store(kNoEnd, std::memory_order_release);
    }

    bool decoded_since_wrap = true;
    while (!eof_) {
        const uint64_t write = write_frame_.load(std::memory_order_relaxed);
        // Frames before the current generation's start are dead: once the render thread has asked
        // for a seek it never reads them again (it skips past them), so their slots are free.
        const uint64_t read = std::max(read_frame_.load(std::memory_order_acquire), worker_gen_start_);
        const uint64_t used = write - read;
        if (used >= ring_frames_) {
            break;
        }
        const auto max_frames = static_cast<int>(std::min<uint64_t>(ring_frames_ - used, kDecodeChunk));
        float** planar = nullptr;
        const long got = reader_->read(&planar, max_frames);
        if (got > 0) {
            for (long j = 0; j < got; ++j) {
                float* frame = &ring_[static_cast<std::size_t>((write + static_cast<uint64_t>(j)) % ring_frames_) *
                                      channels_];
                for (uint32_t c = 0; c < channels_; ++c) {
                    frame[c] = planar[c][j];
                }
            }
            write_frame_.store(write + static_cast<uint64_t>(got), std::memory_order_release);
            decoded_since_wrap = true;
        } else if (got == 0) {
            if (looping_.load(std::memory_order_acquire) && decoded_since_wrap) {
                if (!reader_->seek(0)) {
                    fail("seek failed");
                    return;
                }
                decoded_since_wrap = false;  // guards against spinning on a stream that yields nothing
                continue;
            }
            eof_ = true;
            end_frame_.store(write, std::memory_order_release);
        } else {
            fail("decode error");
            return;
        }
    }
}

// ---- render side ----

void Stream::request_seek(uint64_t frame) noexcept {
    ++render_gen_;
    seek_request_frame_.store(frame, std::memory_order_relaxed);
    seek_request_gen_.store(render_gen_, std::memory_order_release);
    awaiting_ = true;
    base_frame_ = frame;
    end_virtual_ = kUnknownEnd;
}

Stream::Status Stream::prepare_window(int64_t first, int64_t end) noexcept {
    if (awaiting_) {
        if (produced_gen_.load(std::memory_order_acquire) != render_gen_) {
            return Status::Waiting;
        }
        gen_start_ = gen_start_frame_.load(std::memory_order_relaxed);
        read_frame_.store(gen_start_, std::memory_order_release);  // discard data from before the seek
        awaiting_ = false;
        reset_window();
    }

    // Drop frames the resampler no longer needs.
    if (first > window_first_) {
        const auto drop = static_cast<uint32_t>(std::min<int64_t>(first - window_first_, window_len_));
        if (drop > 0) {
            for (uint32_t c = 0; c < channels_; ++c) {
                std::memmove(window_ptrs_[c], window_ptrs_[c] + drop, (window_len_ - drop) * sizeof(float));
            }
            window_first_ += drop;
            window_len_ -= drop;
        }
    }

    const int64_t window_end = window_first_ + window_len_;
    if (end <= window_end) {
        return Status::Ready;
    }
    auto need = static_cast<uint64_t>(end - window_end);
    if (window_len_ + need > window_capacity_) {
        return Status::Starved;  // cannot happen with the engine's sizing; never overrun the window
    }

    const uint64_t read = read_frame_.load(std::memory_order_relaxed);
    const uint64_t available = write_frame_.load(std::memory_order_acquire) - read;
    const uint64_t take = std::min(need, available);
    for (uint64_t i = 0; i < take; ++i) {
        const float* frame = &ring_[static_cast<std::size_t>((read + i) % ring_frames_) * channels_];
        for (uint32_t c = 0; c < channels_; ++c) {
            window_ptrs_[c][window_len_ + i] = frame[c];
        }
    }
    read_frame_.store(read + take, std::memory_order_release);
    window_len_ += static_cast<uint32_t>(take);
    need -= take;
    if (need == 0) {
        return Status::Ready;
    }

    const uint64_t end_frame = end_frame_.load(std::memory_order_acquire);
    if (end_frame != kNoEnd && read + take >= end_frame) {
        end_virtual_ = static_cast<int64_t>(end_frame - gen_start_);
        for (uint32_t c = 0; c < channels_; ++c) {
            std::fill(window_ptrs_[c] + window_len_, window_ptrs_[c] + window_len_ + need, 0.0f);
        }
        window_len_ += static_cast<uint32_t>(need);
        return Status::Ready;
    }
    return Status::Starved;
}

}  // namespace vsa
