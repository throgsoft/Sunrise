#pragma once

#include "runtime.h"

namespace sunrise::state::runtime::detail::bounty {

/** Bounty rewards and source removal form one transaction; ordinary gear dismantles retain
 * their upstream path. No record claim or vendor answer is fabricated for this action. */
struct PendingRedemption : PursuitRedemptionContext {
    PendingRecordRewardGrant grant{};
};

[[nodiscard]] bool prepare_redemption_grant(std::uint64_t source,
                                            std::int32_t quantity,
                                            PendingRecordRewardGrant& grant) noexcept;
[[nodiscard]] bool materialize_redemption_grant(const AccountState& current,
                                                const PendingRecordRewardGrant& grant,
                                                AccountState& after) noexcept;
[[nodiscard]] bool commit_redemption_grant(const PendingRecordRewardGrant& grant) noexcept;

[[nodiscard]] bool stage_rewards(const AccountState& account,
                                 std::span<const DirectRecordReward> rewards,
                                 PendingRecordRewardGrant& mutation,
                                 std::uint64_t reservedSourceSoid = 0,
                                 std::int64_t grantTime = 0) noexcept;
[[nodiscard]] bool prepare_redemption(std::uint64_t sourceInstanceSoid,
                                      std::int32_t expectedQuantity,
                                      PendingRedemption& mutation) noexcept;
[[nodiscard]] bool preview_redemption(const PendingRedemption& mutation,
                                      AccountState& after) noexcept;
[[nodiscard]] bool commit_redemption(PendingRedemption& mutation) noexcept;
} // namespace sunrise::state::runtime::detail::bounty
