#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../account/account_state.h"
#include "../account/inventory/material_identity.h"
#include "../build_data/runtime.h"
#include "bounty_stack_reward_plan.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail::bounty {

/**
 * Credits one stackable profile reward over the rows its bucket already holds.
 *
 * A wallet row saturates at its stack cap rather than refusing the payout, and a reward that
 * spreads takes further rows only while its bucket still owns a free slot. Every credited row
 * becomes its own prepared reward, so one request can report several.
 *
 * @param working Account image the credits are applied to.
 * @param definition Installed identity being credited.
 * @param detail Its configured detail, for the stack cap.
 * @param bucket Its bucket descriptor, which owns the slot range.
 * @param requested Amount the caller asked to credit.
 * @param mutation Receives one prepared reward per credited row.
 * @param rewardCount Running count of prepared rewards, advanced by this call.
 * @param credited Receives the amount actually credited, which may saturate below requested.
 * @return False when the request cannot be planned at all.
 */
[[nodiscard]] inline bool
credit_profile_stacks(AccountState& working,
                      const build_data::items::Definition& definition,
                      const build_data::items::details::Definition& detail,
                      const build_data::inventory::buckets::Descriptor& bucket,
                      std::int32_t requested,
                      PendingRecordRewardGrant& mutation,
                      std::size_t& rewardCount,
                      std::int32_t& credited) noexcept {
    credited = 0;
    if (detail.instancedDefinitionState
            != build_data::items::details::InstancedDefinitionState::stackable
        || build_data::is_profile_action_source(definition.definitionIndex, definition.bucketId)
        || rewardCount > mutation.rewards.size()) {
        return false;
    }
    std::array<StackRow, account::inventory::kProfileItemCapacity> matching{};
    std::size_t matchingCount = 0, used = 0;
    std::int32_t serial = 0;
    for (std::size_t i = 0; i < working.profileItemCount; ++i) {
        const auto& row = working.profileItems[i];
        build_data::items::Definition held{};
        if (!build_data::find_item_definition_hash(row.definitionHash, held)) {
            return false;
        }
        used += held.bucketId == definition.bucketId;
        serial = (std::max)(serial, row.mutationSerial);
        if (row.definitionHash == definition.definitionHash) {
            if (row.instanceSoid != 0) {
                return false;
            }
            matching[matchingCount++] = {i, row.quantity};
        }
    }
    if (used > bucket.slotCount) {
        return false;
    }
    const auto free = (std::min)(working.profileItems.size() - working.profileItemCount,
                                 static_cast<std::size_t>(bucket.slotCount) - used);
    std::array<StackCredit, kRecordRewardGrantCapacity> credits{};
    std::size_t count{};
    const bool multiStack = definition.definitionHash == account::inventory::kEnhancementCoreHash;
    if (!plan_stack_reward(std::span(matching).first(matchingCount),
                           requested,
                           detail.maxStackSize,
                           multiStack,
                           working.profileItemCount,
                           free,
                           std::span(credits).first(mutation.rewards.size() - rewardCount),
                           count,
                           credited)
        || count > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)() - serial)) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& credit = credits[i];
        auto& row = working.profileItems[credit.index];
        if (credit.appended) {
            row = {0, definition.definitionHash, credit.after, ++serial};
            ++working.profileItemCount;
        } else {
            row.quantity = credit.after;
            row.mutationSerial = ++serial;
        }
        auto& reward = mutation.rewards[rewardCount++];
        reward = {};
        reward.definitionHash = definition.definitionHash;
        reward.stateIndex = credit.index;
        reward.quantity = credit.credited;
        reward.afterQuantity = credit.after;
        reward.mutationSerial = row.mutationSerial;
        reward.kind = RecordRewardKind::profileStack;
    }
    return true;
}

} // namespace sunrise::state::runtime::detail::bounty
