#pragma once
#include <swofty/game/base.hpp>

namespace swofty::game {

using Car_Reset_LastFittedUpgrade_t = void(void *inst);
inline constexpr fn<"Car_Reset_LastFittedUpgrade", Car_Reset_LastFittedUpgrade_t> Car_Reset_LastFittedUpgrade{}; // L2 experimental

}
