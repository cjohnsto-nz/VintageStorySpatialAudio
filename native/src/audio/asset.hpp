#pragma once

#include "vsaudio.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace vsa {

/// Immutable audio data shared by voices. Reference counted: the creator holds one reference
/// and every voice another. The last release deletes the asset; the engine only ever drops a
/// voice's reference on its worker thread, never on the render thread.
class Asset {
public:
    /// Decodes or copies `desc` on the calling thread. Returns an asset with one reference.
    static Asset* create(const vsa_asset_desc& desc, uint32_t stream_threshold_ms);

    Asset(const Asset&) = delete;
    Asset& operator=(const Asset&) = delete;

    void add_ref() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] bool streamed() const noexcept { return streamed_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Decoded storage: interleaved int16 at the source rate. Empty when streamed.
    [[nodiscard]] const int16_t* pcm() const noexcept { return pcm_.data(); }
    /// Streamed storage: the encoded Ogg bytes. Empty when decoded.
    [[nodiscard]] const std::vector<uint8_t>& encoded() const noexcept { return encoded_; }

    [[nodiscard]] vsa_asset_info info() const noexcept;

private:
    Asset() = default;
    ~Asset() = default;

    std::atomic<uint32_t> refs_{1};
    uint32_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    uint64_t frames_ = 0;
    bool streamed_ = false;
    std::vector<int16_t> pcm_;
    std::vector<uint8_t> encoded_;
    std::string name_;
};

}  // namespace vsa
