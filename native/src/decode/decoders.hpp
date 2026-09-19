#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vsa::decode {

/// Fully decoded audio: interleaved signed 16-bit PCM at the source rate.
struct DecodedAudio {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> pcm;

    [[nodiscard]] uint64_t frames() const noexcept { return channels == 0 ? 0 : pcm.size() / channels; }
};

/// Largest channel count the engine accepts (the game only uses mono and stereo).
inline constexpr uint32_t kMaxChannels = 2;

[[nodiscard]] bool looks_like_ogg(const uint8_t* data, std::size_t size) noexcept;
[[nodiscard]] bool looks_like_wav(const uint8_t* data, std::size_t size) noexcept;

/// RIFF WAVE: PCM 8/16/24/32-bit, IEEE float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE.
/// Throws vsa::Error (VSA_ERROR_DECODE or VSA_ERROR_UNSUPPORTED).
[[nodiscard]] DecodedAudio decode_wav(const uint8_t* data, std::size_t size);

/// Ogg Vorbis, every logical stream in the file (all links must share rate and channel count).
[[nodiscard]] DecodedAudio decode_ogg(const uint8_t* data, std::size_t size);

/// Incremental Ogg Vorbis decoder over a caller-owned memory buffer that must outlive it.
/// Not thread-safe; used by one thread at a time.
class VorbisReader {
public:
    VorbisReader(const uint8_t* data, std::size_t size);  // throws vsa::Error
    ~VorbisReader();

    VorbisReader(const VorbisReader&) = delete;
    VorbisReader& operator=(const VorbisReader&) = delete;

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] uint64_t frames() const noexcept { return frames_; }

    /// Decodes up to `max_frames`. Sets *planar to per-channel buffers owned by the decoder, valid
    /// until the next call. Returns the frame count, 0 at the end of the stream, or < 0 on a
    /// non-recoverable error (holes in the stream are skipped).
    long read(float*** planar, int max_frames) noexcept;

    /// Positions the decoder at `frame` (clamped to the stream). Returns false on failure.
    bool seek(uint64_t frame) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    uint32_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    uint64_t frames_ = 0;
};

}  // namespace vsa::decode
