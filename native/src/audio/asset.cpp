#include "audio/asset.hpp"

#include "core/error.hpp"
#include "decode/decoders.hpp"

#include <cstring>
#include <string>

namespace vsa {

Asset* Asset::create(const vsa_asset_desc& desc, uint32_t stream_threshold_ms) {
    if (desc.data == nullptr || desc.size == 0) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "asset data must not be empty");
    }
    if (desc.format > VSA_ASSET_FORMAT_PCM_S16) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown asset format " + std::to_string(desc.format));
    }
    if (desc.storage > VSA_ASSET_STORAGE_STREAMED) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "unknown asset storage " + std::to_string(desc.storage));
    }
    static_assert(sizeof(std::size_t) == sizeof(uint64_t), "64-bit targets only");

    const auto* bytes = static_cast<const uint8_t*>(desc.data);
    const auto size = static_cast<std::size_t>(desc.size);

    uint32_t format = desc.format;
    if (format == VSA_ASSET_FORMAT_AUTO) {
        if (decode::looks_like_ogg(bytes, size)) {
            format = VSA_ASSET_FORMAT_OGG_VORBIS;
        } else if (decode::looks_like_wav(bytes, size)) {
            format = VSA_ASSET_FORMAT_WAV;
        } else {
            throw Error(VSA_ERROR_DECODE, "unrecognised audio data (expected Ogg Vorbis or RIFF WAVE)");
        }
    }
    if (desc.storage == VSA_ASSET_STORAGE_STREAMED && format != VSA_ASSET_FORMAT_OGG_VORBIS) {
        throw Error(VSA_ERROR_UNSUPPORTED, "only Ogg Vorbis assets can be streamed");
    }

    auto* asset = new Asset();
    try {
        if (desc.name != nullptr) {
            asset->name_ = desc.name;
        }

        decode::DecodedAudio decoded;
        switch (format) {
            case VSA_ASSET_FORMAT_PCM_S16: {
                if (desc.pcm_channels < 1 || desc.pcm_channels > decode::kMaxChannels) {
                    throw Error(VSA_ERROR_UNSUPPORTED,
                                "PCM asset has " + std::to_string(desc.pcm_channels) + " channels (1 to " +
                                    std::to_string(decode::kMaxChannels) + " supported)");
                }
                if (desc.pcm_sample_rate == 0) {
                    throw Error(VSA_ERROR_INVALID_ARGUMENT, "PCM asset needs pcm_sample_rate");
                }
                const std::size_t frame_bytes = sizeof(int16_t) * desc.pcm_channels;
                if (size % frame_bytes != 0) {
                    throw Error(VSA_ERROR_INVALID_ARGUMENT, "PCM data size is not a whole number of frames");
                }
                decoded.channels = desc.pcm_channels;
                decoded.sample_rate = desc.pcm_sample_rate;
                decoded.pcm.resize(size / sizeof(int16_t));
                std::memcpy(decoded.pcm.data(), bytes, size);
                break;
            }
            case VSA_ASSET_FORMAT_WAV: decoded = decode::decode_wav(bytes, size); break;
            default: {
                bool stream = desc.storage == VSA_ASSET_STORAGE_STREAMED;
                const decode::VorbisReader probe(bytes, size);
                if (desc.storage == VSA_ASSET_STORAGE_AUTO) {
                    stream = probe.frames() * 1000 > uint64_t{stream_threshold_ms} * probe.sample_rate();
                }
                if (stream) {
                    asset->channels_ = probe.channels();
                    asset->sample_rate_ = probe.sample_rate();
                    asset->frames_ = probe.frames();
                    asset->streamed_ = true;
                    asset->layout_ = vorbis_layout(probe.channels());
                    asset->encoded_.assign(bytes, bytes + size);
                } else {
                    decoded = decode::decode_ogg(bytes, size);
                }
                break;
            }
        }

        if (!asset->streamed_) {
            asset->layout_ = format == VSA_ASSET_FORMAT_OGG_VORBIS ? vorbis_layout(decoded.channels)
                                                                   : wave_layout(decoded.channels, decoded.channel_mask);
            asset->channels_ = decoded.channels;
            asset->sample_rate_ = decoded.sample_rate;
            asset->frames_ = decoded.frames();
            asset->pcm_ = std::move(decoded.pcm);
        }
        if (asset->frames_ == 0) {
            throw Error(VSA_ERROR_DECODE, "the audio contains no frames");
        }
        return asset;
    } catch (...) {
        delete asset;
        throw;
    }
}

vsa_asset_info Asset::info() const noexcept {
    vsa_asset_info info{};
    info.struct_size = sizeof info;
    info.channels = channels_;
    info.sample_rate = sample_rate_;
    info.storage = streamed_ ? VSA_ASSET_STORAGE_STREAMED : VSA_ASSET_STORAGE_DECODED;
    info.frames = frames_;
    info.memory_bytes = pcm_.size() * sizeof(int16_t) + encoded_.size();
    info.duration_seconds = sample_rate_ == 0 ? 0.0 : static_cast<double>(frames_) / sample_rate_;
    return info;
}

}  // namespace vsa
