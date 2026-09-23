#pragma once

#include <cstdint>

namespace sunrise::state {
/** Replaces XP only if the observed total still matches, then publishes earned progression. */
[[nodiscard]] bool replace_seasonal_experience(std::int32_t expected,
                                               std::int32_t replacement) noexcept;
/** Clears a manual claim without removing its inventory grant or acquisition flags. */
[[nodiscard]] bool reset_season_pass_reward(std::uint16_t rewardIndex) noexcept;
} // namespace sunrise::state
