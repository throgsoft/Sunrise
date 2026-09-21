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
/** Consumes credited pickup entries; persistent oven balances are never changed.
 * The caller holds an outer transaction until its removal snapshot has been encoded.
 */
[[nodiscard]] bool drain_pickups(std::uint64_t characterSoid, std::size_t& removed) noexcept;
/** Stages the next bounded batch only into an empty FIFO; never credits the balance again. */
[[nodiscard]] bool stage_queued_pickups(std::uint64_t characterSoid, std::size_t& acquired) noexcept;
} // namespace sunrise::state::runtime::detail::dawning
