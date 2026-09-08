#include "dawning_reward_runtime.h"

#include <algorithm>

#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail::dawning {
namespace identity = account::inventory::dawning;

MaterialReward stage_reward(const DirectRecordReward& request,
                            PendingRecordRewardGrant& mutation,
                            PreparedRecordReward& result) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(request.itemDefinitionIndex, item))
        return MaterialReward::refused;
    const auto index = identity::ingredient(item.definitionHash);
    if (index == identity::kIngredientCount) return MaterialReward::none;
    if (!mutation.beforeDawning) {
        State before{};
        if (!read(before)) return MaterialReward::refused;
        mutation.beforeDawning = before;
        mutation.afterDawning = before;
    }
    std::int32_t credited{};
    if (!mutation.afterDawning
        || !credit(*mutation.afterDawning, item.definitionHash, request.quantity, credited))
        return MaterialReward::refused;
    result = {};
    result.definitionHash = item.definitionHash;
    result.stateIndex = index;
    result.quantity = credited;
    result.afterQuantity = mutation.afterDawning->ingredients[index];
    result.kind = RecordRewardKind::accountMaterial;
    return MaterialReward::staged;
}

bool validate_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    if (mutation.rewardCount > mutation.rewards.size()
        || mutation.beforeDawning.has_value() != mutation.afterDawning.has_value())
        return false;
    State expected{};
    if (mutation.beforeDawning && (!read(expected) || expected != *mutation.beforeDawning))
        return false;
    bool material = false;
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        const auto index = identity::ingredient(reward.definitionHash);
        if (reward.kind != RecordRewardKind::accountMaterial) {
            // Reject attempts to turn a pickup wrapper into an inventory grant.
            if (index != identity::kIngredientCount) return false;
            continue;
        }
        material = true;
        if (!mutation.beforeDawning || index == identity::kIngredientCount
            || reward.stateIndex != index || reward.instanceSoid != 0 || reward.inventoryRow != 0
            || reward.mutationSerial != 0 || reward.appendedProfileResident || reward.quantity < 0)
            return false;
        std::int32_t credited{};
        // A full counter is an accepted zero-credit reward; no pickup is announced for it.
        if (!credit(expected, reward.definitionHash, (std::max)(1, reward.quantity), credited)
            || credited != reward.quantity || expected.ingredients[index] != reward.afterQuantity)
            return false;
    }
    return material == mutation.beforeDawning.has_value()
           && (!material || expected == *mutation.afterDawning);
}

bool write_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    if (!validate_rewards(mutation)) return false;
    return !mutation.beforeDawning || write(*mutation.beforeDawning, *mutation.afterDawning);
}
} // namespace sunrise::state::runtime::detail::dawning
