#pragma once

#include <cstdint>

namespace sunrise::state::progression::season_pass {

/** Account progression used by the installed Season of Arrivals pass. */
inline constexpr std::uint16_t kProgressionDefinitionIndex = 40;
/** Repeating HUD bar paired with the Arrivals progression. */
inline constexpr std::uint16_t kHudProgressionDefinitionIndex = 41;

} // namespace sunrise::state::progression::season_pass
