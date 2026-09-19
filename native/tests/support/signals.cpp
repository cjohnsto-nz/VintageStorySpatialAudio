#include "support/signals.hpp"

#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace vsa_test {
namespace {

void put16(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void put_tag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

}  // namespace

std::vector<float> sine(double frequency, double sample_rate, std::size_t frames, float amplitude, uint32_t channels) {
    std::vector<float> out(frames * channels);
    const double w = 2.0 * std::numbers::pi * frequency / sample_rate;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(static_cast<double>(amplitude) * std::sin(w * static_cast<double>(i)));
        for (uint32_t c = 0; c < channels; ++c) {
            out[i * channels + c] = value;
        }
    }
    return out;
}

std::vector<int16_t> to_s16(const std::vector<float>& samples) {
    std::vector<int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double scaled = std::nearbyint(static_cast<double>(samples[i]) * 32768.0);
        out[i] = static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
    }
    return out;
}

std::vector<uint8_t> wav_file(const std::vector<float>& interleaved, uint32_t channels, uint32_t sample_rate,
                              uint32_t bits, bool floating_point) {
    const uint32_t bytes = bits / 8;
    const auto data_size = static_cast<uint32_t>(interleaved.size() * bytes);
    std::vector<uint8_t> out;
    put_tag(out, "RIFF");
    put32(out, 36 + data_size);
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put32(out, 16);
    put16(out, floating_point ? 3 : 1);
    put16(out, channels);
    put32(out, sample_rate);
    put32(out, sample_rate * channels * bytes);
    put16(out, channels * bytes);
    put16(out, bits);
    put_tag(out, "data");
    put32(out, data_size);
    for (const float sample : interleaved) {
        const double x = std::clamp(static_cast<double>(sample), -1.0, 1.0);
        if (floating_point) {
            uint8_t raw[8];
            if (bits == 32) {
                const auto f = static_cast<float>(x);
                std::memcpy(raw, &f, 4);
            } else {
                std::memcpy(raw, &x, 8);
            }
            out.insert(out.end(), raw, raw + bytes);
            continue;
        }
        switch (bits) {
            case 8: out.push_back(static_cast<uint8_t>(std::lround(x * 127.0) + 128)); break;
            case 16: put16(out, static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 32767.0)))); break;
            case 24: {
                const auto v = static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 8388607.0)));
                out.push_back(static_cast<uint8_t>(v & 0xFF));
                out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
                break;
            }
            default: put32(out, static_cast<uint32_t>(static_cast<int32_t>(std::llround(x * 2147483647.0)))); break;
        }
    }
    return out;
}

std::vector<uint8_t> ogg_file(const std::vector<float>& interleaved, uint32_t channels, uint32_t sample_rate,
                              float quality) {
    std::vector<uint8_t> out;
    const auto append_page = [&](const ogg_page& page) {
        out.insert(out.end(), page.header, page.header + page.header_len);
        out.insert(out.end(), page.body, page.body + page.body_len);
    };

    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, static_cast<long>(channels), static_cast<long>(sample_rate), quality) != 0) {
        vorbis_info_clear(&info);
        throw std::runtime_error("vorbis_encode_init_vbr failed");
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_dsp_state dsp;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block block;
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream;
    ogg_stream_init(&stream, 0x5A5A);

    ogg_packet header;
    ogg_packet header_comment;
    ogg_packet header_code;
    vorbis_analysis_headerout(&dsp, &comment, &header, &header_comment, &header_code);
    ogg_stream_packetin(&stream, &header);
    ogg_stream_packetin(&stream, &header_comment);
    ogg_stream_packetin(&stream, &header_code);
    ogg_page page;
    while (ogg_stream_flush(&stream, &page) != 0) {
        append_page(page);
    }

    const std::size_t frames = interleaved.size() / channels;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t n = std::min<std::size_t>(1024, frames - pos);
        if (n == 0) {
            vorbis_analysis_wrote(&dsp, 0);
        } else {
            float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(n));
            for (std::size_t i = 0; i < n; ++i) {
                for (uint32_t c = 0; c < channels; ++c) {
                    buffer[c][i] = interleaved[(pos + i) * channels + c];
                }
            }
            vorbis_analysis_wrote(&dsp, static_cast<int>(n));
            pos += n;
        }
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp, &packet) != 0) {
                ogg_stream_packetin(&stream, &packet);
                while (ogg_stream_pageout(&stream, &page) != 0) {
                    append_page(page);
                }
            }
        }
        if (n == 0) {
            break;
        }
    }
    while (ogg_stream_flush(&stream, &page) != 0) {
        append_page(page);
    }

    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return out;
}

std::vector<float> channel(const std::vector<float>& interleaved, uint32_t channels, uint32_t index) {
    std::vector<float> out(interleaved.size() / channels);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = interleaved[i * channels + index];
    }
    return out;
}

double rms(const float* x, std::size_t n) {
    if (n == 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(n));
}

double peak(const float* x, std::size_t n) {
    double value = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        value = std::max(value, std::abs(static_cast<double>(x[i])));
    }
    return value;
}

double to_db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

double SineFit::thd_n_db() const { return to_db(residual_rms / (amplitude / std::numbers::sqrt2)); }

SineFit fit_sine(const float* x, std::size_t n, double frequency, double sample_rate) {
    // Normal equations for [sin, cos, 1].
    const double w = 2.0 * std::numbers::pi * frequency / sample_rate;
    std::array<std::array<double, 4>, 3> m{};
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i);
        const std::array<double, 3> basis{std::sin(w * t), std::cos(w * t), 1.0};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                m[r][c] += basis[r] * basis[c];
            }
            m[r][3] += basis[r] * static_cast<double>(x[i]);
        }
    }
    // Gauss-Jordan elimination (3x3, well conditioned for a few periods of data).
    for (std::size_t p = 0; p < 3; ++p) {
        const double pivot = m[p][p];
        for (std::size_t c = 0; c < 4; ++c) {
            m[p][c] /= pivot;
        }
        for (std::size_t r = 0; r < 3; ++r) {
            if (r != p) {
                const double factor = m[r][p];
                for (std::size_t c = 0; c < 4; ++c) {
                    m[r][c] -= factor * m[p][c];
                }
            }
        }
    }
    const double a = m[0][3];
    const double b = m[1][3];
    const double offset = m[2][3];
    double residual = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i);
        const double e = static_cast<double>(x[i]) - (a * std::sin(w * t) + b * std::cos(w * t) + offset);
        residual += e * e;
    }
    SineFit fit;
    fit.amplitude = std::hypot(a, b);
    fit.residual_rms = std::sqrt(residual / static_cast<double>(n));
    return fit;
}

double max_step(const float* x, std::size_t n) {
    double step = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        step = std::max(step, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    }
    return step;
}

}  // namespace vsa_test
