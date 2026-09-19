#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vsa_test {

/// Interleaved sine, the same signal on every channel.
std::vector<float> sine(double frequency, double sample_rate, std::size_t frames, float amplitude = 0.5f,
                        uint32_t channels = 1);

std::vector<int16_t> to_s16(const std::vector<float>& samples);

/// A RIFF WAVE file. `bits`: 8/16/24/32 for PCM, 32/64 for float.
std::vector<uint8_t> wav_file(const std::vector<float>& interleaved, uint32_t channels, uint32_t sample_rate,
                              uint32_t bits, bool floating_point = false);

/// An Ogg Vorbis file encoded with libvorbis (VBR at `quality`, -0.1..1).
std::vector<uint8_t> ogg_file(const std::vector<float>& interleaved, uint32_t channels, uint32_t sample_rate,
                              float quality = 0.6f);

/// One channel of an interleaved buffer.
std::vector<float> channel(const std::vector<float>& interleaved, uint32_t channels, uint32_t index);

double rms(const float* x, std::size_t n);
double peak(const float* x, std::size_t n);
inline double rms(const std::vector<float>& x) { return rms(x.data(), x.size()); }
inline double peak(const std::vector<float>& x) { return peak(x.data(), x.size()); }
double to_db(double ratio);

/// Least-squares fit of a*sin + b*cos + c at a known frequency.
struct SineFit {
    double amplitude = 0.0;
    double residual_rms = 0.0;
    /// Residual relative to the fitted sine's RMS, in dB (THD+N for a pure tone).
    [[nodiscard]] double thd_n_db() const;
};
SineFit fit_sine(const float* x, std::size_t n, double frequency, double sample_rate);

/// Largest absolute difference between consecutive samples: a click detector.
double max_step(const float* x, std::size_t n);

}  // namespace vsa_test
