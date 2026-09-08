#pragma once
#include "dawning_oven_runtime.h"

namespace sunrise::state::runtime::detail::dawning {
enum class MaterialReward { none, staged, refused };
[[nodiscard]] MaterialReward stage_reward(const DirectRecordReward& request,
                                          PendingRecordRewardGrant& mutation,
                                          PreparedRecordReward& result) noexcept;
[[nodiscard]] bool validate_rewards(const PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] bool write_rewards(const PendingRecordRewardGrant& mutation) noexcept;
} // namespace sunrise::state::runtime::detail::dawning
