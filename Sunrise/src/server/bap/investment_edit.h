#pragma once

#include <cstdint>

namespace sunrise::server::bap {
/** Edits an observed stack and schedules the normal account update after a successful save. */
[[nodiscard]] bool set_inventory_quantity(std::uint64_t character,
                                          std::uint64_t instance,
                                          std::uint32_t hash,
                                          std::int32_t serial,
                                          std::int32_t expected,
                                          std::int32_t quantity) noexcept;
/** Clears a manual claim and republishes flags; acquired items are retained. */
[[nodiscard]] bool reset_season_pass_claim(std::uint16_t rewardIndex) noexcept;
/** Replaces an observed XP total and republishes earned progression and flags. */
[[nodiscard]] bool set_seasonal_experience(std::int32_t expected,
                                           std::int32_t replacement) noexcept;
} // namespace sunrise::server::bap
