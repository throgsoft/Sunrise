#pragma once

#include <cstdint>

namespace sunrise::server::bap {
/** Validates and queues an acquisition through the installed item's State planner. */
[[nodiscard]] bool queue_item_reward(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
/** Queues a native claim on the selected character's authenticated Family-4 subscription. */
[[nodiscard]] bool queue_season_pass_reward(std::uint16_t rewardIndex) noexcept;
/** Reports whether a claim is queued for this row, and whether any claim blocks another. */
[[nodiscard]] bool season_pass_claim_pending(std::uint16_t rewardIndex, bool& busy) noexcept;
/** True when native investment updates can be delivered. */
[[nodiscard]] bool investment_connected() noexcept;
} // namespace sunrise::server::bap
