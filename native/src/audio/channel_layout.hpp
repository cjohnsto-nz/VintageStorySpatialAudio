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
    Other,
};

inline constexpr uint32_t kMaxOutputChannels = 8;

/// The order Steam Audio's panning effect writes for 2/4/6/8 channels (stereo, quad, 5.1, 7.1).
/// The engine mixes in this order; channels 0/1 are front left/right in every layout.
[[nodiscard]] constexpr std::array<Speaker, kMaxOutputChannels> steam_layout(uint32_t channels) noexcept {
    using enum Speaker;
    switch (channels) {
        case 4: return {FrontLeft, FrontRight, BackLeft, BackRight, Other, Other, Other, Other};
        case 6: return {FrontLeft, FrontRight, FrontCentre, Lfe, BackLeft, BackRight, Other, Other};
        case 8: return {FrontLeft, FrontRight, FrontCentre, Lfe, BackLeft, BackRight, SideLeft, SideRight};
        default: return {FrontLeft, FrontRight, Other, Other, Other, Other, Other, Other};
    }
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
