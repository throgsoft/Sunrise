#pragma once
#include "dawning_oven_runtime.h"

namespace sunrise::state::runtime::detail::dawning {
enum class MaterialReward { none, staged, refused };
[[nodiscard]] MaterialReward stage_reward(CharacterState& character,
                                          const DirectRecordReward& request,
                                          PendingRecordRewardGrant& mutation,
                                          PreparedRecordReward& result) noexcept;
[[nodiscard]] bool validate_rewards(const PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] bool write_rewards(const PendingRecordRewardGrant& mutation) noexcept;
/** Stages the next bounded batch, evicting the oldest rows a full FIFO must drop to admit
 * them; never credits the balance again. */
[[nodiscard]] bool stage_queued_pickups(std::uint64_t characterSoid,
                                        std::size_t& acquired) noexcept;
} // namespace sunrise::state::runtime::detail::dawning
