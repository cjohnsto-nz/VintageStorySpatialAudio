#include "decode/decoders.hpp"

#include "core/error.hpp"

// vorbisfile.h otherwise defines unused static stdio callbacks in every includer.
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace vsa::decode {
namespace {

uint16_t le16(const uint8_t* p) noexcept { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int16_t to_s16(double x) noexcept {
    const double scaled = std::nearbyint(x * 32768.0);
    return static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
}

bool chunk_is(const uint8_t* p, const char* id) noexcept { return std::memcmp(p, id, 4) == 0; }

std::string channel_error(uint32_t channels) {
    return std::to_string(channels) + " channels (1 to " + std::to_string(kMaxChannels) + " are supported)";
}

// ---- vorbisfile over memory ----

struct MemoryCursor {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

std::size_t memory_read(void* ptr, std::size_t size, std::size_t count, void* source) {
    auto* cursor = static_cast<MemoryCursor*>(source);
    if (size == 0) {
        return 0;
    }
    const std::size_t n = std::min(count, (cursor->size - cursor->pos) / size);
    std::memcpy(ptr, cursor->data + cursor->pos, n * size);
    cursor->pos += n * size;
    return n;
}

int memory_seek(void* source, ogg_int64_t offset, int whence) {
    auto* cursor = static_cast<MemoryCursor*>(source);
    ogg_int64_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<ogg_int64_t>(cursor->pos); break;
        case SEEK_END: base = static_cast<ogg_int64_t>(cursor->size); break;
        default: return -1;
    }
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(cursor->size)) {
        return -1;
    }
    cursor->pos = static_cast<std::size_t>(target);
    return 0;
}

long memory_tell(void* source) { return static_cast<long>(static_cast<MemoryCursor*>(source)->pos); }

const char* vorbis_error_name(int code) noexcept {
    switch (code) {
        case OV_EREAD: return "read error";
        case OV_EFAULT: return "internal fault";
        case OV_EIMPL: return "unsupported feature";
        case OV_EINVAL: return "invalid argument";
        case OV_ENOTVORBIS: return "not Vorbis data";
        case OV_EBADHEADER: return "bad Vorbis header";
        case OV_EVERSION: return "Vorbis version mismatch";
        case OV_EBADLINK: return "bad link";
        case OV_ENOSEEK: return "stream not seekable";
        default: return "error";
    }
}

}  // namespace

bool looks_like_ogg(const uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 4 && chunk_is(data, "OggS");
}

bool looks_like_wav(const uint8_t* data, std::size_t size) noexcept {
    return data != nullptr && size >= 12 && chunk_is(data, "RIFF") && chunk_is(data + 8, "WAVE");
}

DecodedAudio decode_wav(const uint8_t* data, std::size_t size) {
    if (!looks_like_wav(data, size)) {
        throw Error(VSA_ERROR_DECODE, "not a RIFF WAVE file");
    }

    bool have_format = false;
    uint16_t format = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t bits = 0;
    uint32_t mask = 0;
    const uint8_t* samples = nullptr;
    std::size_t sample_bytes = 0;

    std::size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* chunk = data + pos;
        const uint32_t chunk_size = le32(chunk + 4);
        const std::size_t body = pos + 8;
        const std::size_t available = std::min<std::size_t>(chunk_size, size - body);  // tolerate truncation
        if (chunk_is(chunk, "fmt ")) {
            if (available < 16) {
                throw Error(VSA_ERROR_DECODE, "WAV fmt chunk too short");
            }
            format = le16(data + body);
            channels = le16(data + body + 2);
            sample_rate = le32(data + body + 4);
            bits = le16(data + body + 14);
            if (format == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE: the real format is the SubFormat GUID's first 2 bytes
                if (available < 26) {
                    throw Error(VSA_ERROR_DECODE, "WAV extensible fmt chunk too short");
                }
                format = le16(data + body + 24);
                mask = le32(data + body + 20);
            }
            have_format = true;
        } else if (chunk_is(chunk, "data")) {
            samples = data + body;
            sample_bytes = available;
        }
        if (samples != nullptr && have_format) {
            break;
        }
        const std::size_t advance = 8 + static_cast<std::size_t>(chunk_size) + (chunk_size & 1u);
        if (advance > size - pos) {
            break;
        }
        pos += advance;
    }

    if (!have_format) {
        throw Error(VSA_ERROR_DECODE, "WAV has no fmt chunk");
    }
    if (samples == nullptr) {
        throw Error(VSA_ERROR_DECODE, "WAV has no data chunk");
    }
    if (channels < 1 || channels > kMaxChannels) {
        throw Error(VSA_ERROR_UNSUPPORTED, "WAV has " + channel_error(channels));
    }
    if (sample_rate == 0) {
        throw Error(VSA_ERROR_DECODE, "WAV sample rate is 0");
    }
    const bool is_pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool is_float = format == 3 && (bits == 32 || bits == 64);
    if (!is_pcm && !is_float) {
        throw Error(VSA_ERROR_UNSUPPORTED,
                    "WAV format " + std::to_string(format) + " with " + std::to_string(bits) + " bits is not supported");
    }

    const std::size_t bytes = bits / 8;
    const std::size_t frame_bytes = bytes * channels;
    const std::size_t count = (sample_bytes / frame_bytes) * channels;

    DecodedAudio out;
    out.channels = channels;
    out.sample_rate = sample_rate;
    out.channel_mask = mask;
    out.pcm.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t* p = samples + i * bytes;
        int16_t value = 0;
        if (is_float) {
            if (bits == 32) {
                float f = 0.0f;
                std::memcpy(&f, p, 4);
                value = to_s16(static_cast<double>(f));
            } else {
                double d = 0.0;
                std::memcpy(&d, p, 8);
                value = to_s16(d);
            }
        } else {
            switch (bits) {
                case 8: value = static_cast<int16_t>((static_cast<int>(p[0]) - 128) * 256); break;
                case 16: value = static_cast<int16_t>(le16(p)); break;
                case 24: value = static_cast<int16_t>(p[1] | (p[2] << 8)); break;  // drop the low byte
                default: value = static_cast<int16_t>(le16(p + 2)); break;         // 32-bit: keep the top 16
            }
        }
        out.pcm[i] = value;
    }
    return out;
}

DecodedAudio decode_ogg(const uint8_t* data, std::size_t size) {
    VorbisReader reader(data, size);
    DecodedAudio out;
    out.channels = reader.channels();
    out.sample_rate = reader.sample_rate();
    // Header-reported length; never trust it beyond what the encoded size could plausibly hold.
    out.pcm.reserve(static_cast<std::size_t>(std::min<uint64_t>(reader.frames(), uint64_t{size} * 64)) * out.channels);

    for (;;) {
        float** planar = nullptr;
        const long got = reader.read(&planar, 4096);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            throw Error(VSA_ERROR_DECODE, "Ogg Vorbis stream is corrupt");
        }
        for (long j = 0; j < got; ++j) {
            for (uint32_t c = 0; c < out.channels; ++c) {
                out.pcm.push_back(to_s16(static_cast<double>(planar[c][j])));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

struct VorbisReader::State {
    MemoryCursor cursor;
    OggVorbis_File file{};
    bool open = false;
};

VorbisReader::VorbisReader(const uint8_t* data, std::size_t size) : state_(std::make_unique<State>()) {
    if (!looks_like_ogg(data, size)) {
        throw Error(VSA_ERROR_DECODE, "not an Ogg file");
    }
    state_->cursor = MemoryCursor{data, size, 0};
    const ov_callbacks callbacks{&memory_read, &memory_seek, nullptr, &memory_tell};
    if (const int result = ov_open_callbacks(&state_->cursor, &state_->file, nullptr, 0, callbacks); result != 0) {
        throw Error(VSA_ERROR_DECODE, std::string("Ogg Vorbis open failed: ") + vorbis_error_name(result));
    }
    state_->open = true;

    const vorbis_info* first = ov_info(&state_->file, 0);
    if (first == nullptr) {
        throw Error(VSA_ERROR_DECODE, "Ogg Vorbis stream has no info header");
    }
    const long links = ov_streams(&state_->file);
    for (long link = 1; link < links; ++link) {
        const vorbis_info* info = ov_info(&state_->file, static_cast<int>(link));
        if (info == nullptr || info->channels != first->channels || info->rate != first->rate) {
            throw Error(VSA_ERROR_UNSUPPORTED, "chained Ogg streams with different formats are not supported");
        }
    }
    if (first->channels < 1 || static_cast<uint32_t>(first->channels) > kMaxChannels) {
        throw Error(VSA_ERROR_UNSUPPORTED, "Ogg Vorbis has " + channel_error(static_cast<uint32_t>(first->channels)));
    }
    if (first->rate <= 0) {
        throw Error(VSA_ERROR_DECODE, "Ogg Vorbis sample rate is invalid");
    }
    channels_ = static_cast<uint32_t>(first->channels);
    sample_rate_ = static_cast<uint32_t>(first->rate);
    const ogg_int64_t total = ov_pcm_total(&state_->file, -1);
    frames_ = total > 0 ? static_cast<uint64_t>(total) : 0;
}

VorbisReader::~VorbisReader() {
    if (state_ && state_->open) {
        ov_clear(&state_->file);
    }
}

long VorbisReader::read(float*** planar, int max_frames) noexcept {
    for (;;) {
        int bitstream = 0;
        const long got = ov_read_float(&state_->file, planar, max_frames, &bitstream);
        if (got != OV_HOLE) {
            return got;
        }
    }
}

bool VorbisReader::seek(uint64_t frame) noexcept {
    const auto target = static_cast<ogg_int64_t>(std::min(frame, frames_));
    return ov_pcm_seek(&state_->file, target) == 0;
}

}  // namespace vsa::decode
