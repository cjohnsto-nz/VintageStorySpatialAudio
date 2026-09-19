#pragma once

#include <array>
#include <cstdint>

namespace vsa {

/// Speaker positions the engine renders to.
enum class Speaker : uint8_t {
    FrontLeft,
    FrontRight,
    FrontCentre,
    Lfe,
    BackLeft,
    BackRight,
    SideLeft,
    SideRight,
    TopFrontLeft,
    TopFrontRight,
    TopBackLeft,
    TopBackRight,
    Other,
};

inline constexpr uint32_t kMaxOutputChannels = 12;

/// Layouts the engine renders: stereo, quad, 5.1, 7.1 and 7.1.4.
[[nodiscard]] constexpr bool is_supported_layout(uint32_t channels) noexcept {
    return channels == 2 || channels == 4 || channels == 6 || channels == 8 || channels == 12;
}

/// The engine's channel order for 2/4/6/8/12 channels: Steam Audio's panning order for stereo,
/// quad, 5.1 and 7.1, and 7.1 plus the four heights for 7.1.4 (the Windows channel-mask order).
/// Channels 0/1 are front left/right in every layout.
[[nodiscard]] constexpr std::array<Speaker, kMaxOutputChannels> steam_layout(uint32_t channels) noexcept {
    using enum Speaker;
    std::array<Speaker, kMaxOutputChannels> layout{};
    layout.fill(Other);
    layout[0] = FrontLeft;
    layout[1] = FrontRight;
    switch (channels) {
        case 4:
            layout[2] = BackLeft;
            layout[3] = BackRight;
            break;
        case 12:
            layout[8] = TopFrontLeft;
            layout[9] = TopFrontRight;
            layout[10] = TopBackLeft;
            layout[11] = TopBackRight;
            [[fallthrough]];
        case 8:
            layout[6] = SideLeft;
            layout[7] = SideRight;
            [[fallthrough]];
        case 6:
            layout[2] = FrontCentre;
            layout[3] = Lfe;
            layout[4] = BackLeft;
            layout[5] = BackRight;
            break;
        default: break;
    }
    return layout;
}

/// For each engine channel (Steam order), the device channel it goes to, or -1 if the device has
/// no such speaker. A 5.1 device may call its surrounds "side" rather than "back" (and a 7.1 one
/// may lack either): each falls back to the other.
[[nodiscard]] constexpr std::array<int, kMaxOutputChannels> map_to_device(uint32_t channels,
                                                                          const Speaker* device) noexcept {
    std::array<int, kMaxOutputChannels> map{};
    map.fill(-1);
    const std::array<Speaker, kMaxOutputChannels> engine = steam_layout(channels);
    std::array<bool, kMaxOutputChannels> taken{};
    const auto find = [&](Speaker wanted) {
        for (uint32_t d = 0; d < channels; ++d) {
            if (!taken[d] && device[d] == wanted) {
                return static_cast<int>(d);
            }
        }
        return -1;
    };
    const auto alternative = [](Speaker s) {
        switch (s) {
            case Speaker::BackLeft: return Speaker::SideLeft;
            case Speaker::BackRight: return Speaker::SideRight;
            case Speaker::SideLeft: return Speaker::BackLeft;
            case Speaker::SideRight: return Speaker::BackRight;
            default: return Speaker::Other;
        }
    };
    // Exact matches first, so an alternative never steals a speaker another channel owns.
    for (uint32_t e = 0; e < channels; ++e) {
        if (engine[e] != Speaker::Other) {
            map[e] = find(engine[e]);
            if (map[e] >= 0) {
                taken[static_cast<uint32_t>(map[e])] = true;
            }
        }
    }
    for (uint32_t e = 0; e < channels; ++e) {
        if (map[e] < 0 && alternative(engine[e]) != Speaker::Other) {
            map[e] = find(alternative(engine[e]));
            if (map[e] >= 0) {
                taken[static_cast<uint32_t>(map[e])] = true;
            }
        }
    }
    return map;
}

}  // namespace vsa
