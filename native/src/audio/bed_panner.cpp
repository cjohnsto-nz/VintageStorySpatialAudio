#include "audio/bed_panner.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa {

namespace {

constexpr float kMinusThreeDb = 0.70710678f;

}  // namespace

BedPanner::BedPanner(uint32_t channels) : channels_(std::min(channels, kMaxOutputChannels)) {
    const auto layout = steam_layout(channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
        if (layout[c] == Speaker::Lfe) {
            lfe_ = static_cast<int>(c);
        }
    }
    if (channels_ >= 4) {
        vbap_ = std::make_unique<Vbap>(channels_, kSurroundAzimuth);
    }
}

void BedPanner::gains(const SourceChannel& channel, float* out) const noexcept {
    std::fill(out, out + channels_, 0.0f);
    if (channel.lfe) {
        if (lfe_ >= 0) {
            out[lfe_] = 1.0f;
        } else {
            out[0] = kMinusThreeDb;
            out[1] = kMinusThreeDb;
        }
        return;
    }

    if (vbap_ != nullptr) {
        float direction[3];
        channel.direction(direction);
        vbap_->gains(direction, out);
        return;
    }

    // Stereo: the front pair is at +-30 degrees; anything wider plays from its side's speaker.
    // A channel behind is mirrored to the front (a back centre plays in the middle) at -3 dB.
    float azimuth = std::remainder(channel.azimuth, 360.0f);
    float weight = 1.0f;
    if (std::abs(azimuth) > 90.0f) {
        azimuth = std::copysign(180.0f - std::abs(azimuth), azimuth);
        weight = kMinusThreeDb;
    }
    const float pan = std::clamp(azimuth / 30.0f, -1.0f, 1.0f);  // -1 left .. 1 right
    const float angle = (pan + 1.0f) * std::numbers::pi_v<float> / 4.0f;
    out[0] = std::cos(angle) * weight;
    out[1] = std::sin(angle) * weight;
}

}  // namespace vsa
