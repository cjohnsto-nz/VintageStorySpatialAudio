#pragma once

#include "steam/steam_context.hpp"
#include "vsaudio.h"

namespace vsa::steam {

/// End-to-end check of the native stack on this machine: scene creation with the
/// active ray tracer, static mesh upload, simulator + source setup and a direct
/// occlusion simulation for a blocked and a clear path.
[[nodiscard]] vsa_self_test_report run_self_test(const SteamContext& steam);

}  // namespace vsa::steam
