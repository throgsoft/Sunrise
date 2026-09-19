#pragma once

#include <cstdint>
#include <span>

#include "runtime.h"

namespace sunrise::state::runtime::detail {

/** The source is already removed from the account passed for a bounty redemption. */
enum class RewardPlacementPolicy : std::uint8_t { ordinary, bountyRedemption };

struct RewardPlacementContext {
    RewardPlacementPolicy policy{RewardPlacementPolicy::ordinary};
    std::uint64_t reservedSourceSoid{};
    std::int64_t grantTime{};
};

[[nodiscard]] bool stage_reward_placement(const AccountState& account,
                                          std::span<const DirectRecordReward> rewards,
                                          std::uint16_t claimedRecordIndex,
                                          RewardPlacementContext context,
                                          PendingRecordRewardGrant& mutation) noexcept;

} // namespace sunrise::state::runtime::detail
