#pragma once

#include "audio/channel_layout.hpp"
#include "audio/source_layout.hpp"
#include "audio/vbap.hpp"

#include <cstdint>
#include <memory>

namespace vsa {

/// Speaker gains for a bed: an unpositioned sound of more than two channels (a weather mod's 5.1
/// rain). Each channel plays from its own speaker direction, head-locked like the speakers
/// themselves, instead of being folded onto the front pair.
///
/// - Layouts of four speakers or more: VBAP over the real speakers, with a lone surround pair
///   (quad, 5.1) at 110 degrees, where 5.1 content has its surrounds. A channel meant for a
///   speaker the layout has plays from that speaker alone (a 5.1 bed on 5.1 is copied through);
///   5.1 surrounds on 7.1 sit between the sides and the backs.
/// - Stereo: panned by azimuth, back channels mirrored to the front at -3 dB (as ITU-R BS.775's
///   downmix weights the surrounds).
/// - The LFE goes to the LFE channel where the layout has one, otherwise to the front pair.
///
/// Gains are power-normalised per channel.
class BedPanner {
public:
    /// For the engine's layout of `channels` output channels. Allocates.
    explicit BedPanner(uint32_t channels);

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }

    /// One output gain per channel (engine order, `channels()` of them) for a source channel.
    /// Never allocates.
    void gains(const SourceChannel& channel, float* out) const noexcept;

private:
    uint32_t channels_;
    int lfe_ = -1;
    std::unique_ptr<Vbap> vbap_;  // four speakers or more
};

}  // namespace vsa
