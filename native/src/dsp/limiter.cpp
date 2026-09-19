#include "dsp/limiter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa::dsp {

void Limiter::prepare(uint32_t sample_rate, float lookahead_ms, float release_ms, float ceiling_db) {
    ceiling_ = std::pow(10.0f, ceiling_db / 20.0f);
    window_ = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(lookahead_ms) * 0.001 * sample_rate)));
    delay_ = kDetectorLag + window_ - 1;
    release_coef_ = static_cast<float>(1.0 - std::exp(-1.0 / (static_cast<double>(release_ms) * 0.001 * sample_rate)));

    // Interpolator for the points m + p/4 (p = 1..3) from x[m - 3 .. m + 4]: a Hann-windowed
    // sinc over +-4 samples, normalised to unity DC gain per phase.
    for (int p = 0; p < kPhases; ++p) {
        const double phi = (p + 1) / 4.0;
        double sum = 0.0;
        std::array<double, kTaps> taps{};
        for (int k = 0; k < kTaps; ++k) {
            const double d = phi + 3.0 - k;  // distance from x[m - 3 + k] to the point
            const double x = d / 4.0;
            const double arg = std::numbers::pi * d;
            const double sinc = std::abs(d) < 1e-12 ? 1.0 : std::sin(arg) / arg;
            const double w = std::abs(x) >= 1.0 ? 0.0 : 0.5 + 0.5 * std::cos(std::numbers::pi * x);
            taps[static_cast<std::size_t>(k)] = sinc * w;
            sum += sinc * w;
        }
        for (int k = 0; k < kTaps; ++k) {
            fir_[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)] =
                static_cast<float>(taps[static_cast<std::size_t>(k)] / sum);
        }
    }

    min_values_.assign(window_ + 1, 1.0f);
    min_index_.assign(window_ + 1, 0);
    box_.assign(window_, 1.0f);
    for (auto& line : lines_) {
        line.assign(delay_ + 1, 0.0f);
    }
    reset();
}

void Limiter::reset() noexcept {
    for (auto& history : hist_) {
        history.fill(0.0f);
    }
    min_head_ = 0;
    min_size_ = 0;
    release_state_ = 1.0f;
    std::fill(box_.begin(), box_.end(), 1.0f);
    box_pos_ = 0;
    box_sum_ = static_cast<double>(window_);
    for (auto& line : lines_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    delay_pos_ = 0;
    sample_index_ = 0;
}

float Limiter::process(float* const* channels, uint32_t count, uint32_t frames) noexcept {
    count = std::min(count, kMaxChannels);
    const auto min_capacity = static_cast<uint32_t>(min_values_.size());
    const auto delay_size = static_cast<uint32_t>(lines_[0].size());
    float smallest = 1.0f;

    for (uint32_t j = 0; j < frames; ++j) {
        // 1. True-peak estimate for sample m = n - kDetectorLag, linked across channels.
        float peak = 0.0f;
        for (uint32_t c = 0; c < count; ++c) {
            auto& history = hist_[c];
            std::copy(history.begin() + 1, history.end(), history.begin());
            history[kTaps - 1] = channels[c][j];
            peak = std::max(peak, std::abs(history[3]));
            for (const auto& fir : fir_) {
                float y = 0.0f;
                for (std::size_t k = 0; k < kTaps; ++k) {
                    y += fir[k] * history[k];
                }
                peak = std::max(peak, std::abs(y));
            }
        }

        // 2. Target gain.
        const float target = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // 3a. Minimum over the last W targets (monotonic deque).
        while (min_size_ > 0) {
            const uint32_t back = (min_head_ + min_size_ - 1) % min_capacity;
            if (min_values_[back] < target) {
                break;
            }
            --min_size_;
        }
        const uint32_t slot = (min_head_ + min_size_) % min_capacity;
        min_values_[slot] = target;
        min_index_[slot] = sample_index_;
        ++min_size_;
        while (min_index_[min_head_] + window_ <= sample_index_) {
            min_head_ = (min_head_ + 1) % min_capacity;
            --min_size_;
        }
        const float held = min_values_[min_head_];

        // 3b. Instant attack, exponential release.
        if (held < release_state_) {
            release_state_ = held;
        } else {
            release_state_ += (held - release_state_) * release_coef_;
        }

        // 3c. Moving average over W.
        box_sum_ += static_cast<double>(release_state_) - static_cast<double>(box_[box_pos_]);
        box_[box_pos_] = release_state_;
        if (++box_pos_ == window_) {
            box_pos_ = 0;
            // Re-sum once per window so rounding cannot accumulate.
            double sum = 0.0;
            for (const float value : box_) {
                sum += static_cast<double>(value);
            }
            box_sum_ = sum;
        }
        // Divide in double: a full window of 1.0 must give exactly 1 (bit-exact pass-through).
        const float gain = std::min(1.0f, static_cast<float>(box_sum_ / window_));
        smallest = std::min(smallest, gain);

        // Delay the audio so the gain lands on the sample it was computed for.
        const uint32_t oldest = delay_pos_ + 1 == delay_size ? 0 : delay_pos_ + 1;
        for (uint32_t c = 0; c < count; ++c) {
            std::vector<float>& line = lines_[c];
            line[delay_pos_] = channels[c][j];
            channels[c][j] = line[oldest] * gain;
        }
        delay_pos_ = oldest;

        ++sample_index_;
    }
    return smallest;
}

}  // namespace vsa::dsp
