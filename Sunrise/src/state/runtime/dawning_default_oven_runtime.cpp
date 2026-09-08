#include "dawning_default_oven_runtime.h"

#include <array>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

#include "../account/inventory/dawning_oven_state.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
DawningOvenBootstrapResult ensure_default_dawning_oven() noexcept {
    namespace store = investment::store;
    namespace items = build_data::items;
    using namespace runtime::detail;
    items::Definition oven{};
    items::details::Definition detail{};
    if (!build_data::find_item_definition_hash(account::inventory::dawning::kOvenHash, oven)
        || !build_data::find_configured_item_detail(oven.definitionIndex, detail))
        return {DawningOvenBootstrapStatus::notReady, false};
    if (oven.definitionHash != account::inventory::dawning::kOvenHash
        || detail.definitionIndex != oven.definitionIndex
        || detail.definitionHash != oven.definitionHash || detail.bucketId != oven.bucketId
        || detail.ordinarySocketCount != 5
        || detail.ordinarySocketState != items::details::OrdinarySocketState::present
        || detail.instancedDefinitionState != items::details::InstancedDefinitionState::instanced
        || detail.equipmentSlot.has_value())
        return {DawningOvenBootstrapStatus::refused, false};
    auto snapshot = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    if (!snapshot) return {DawningOvenBootstrapStatus::refused, false};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*snapshot) || !account::valid(*snapshot))
        return {DawningOvenBootstrapStatus::refused, false};
    const auto selected = selected_character_index(*snapshot);
    if (selected >= snapshot->characterCount) return {DawningOvenBootstrapStatus::notReady, false};
    const auto& character = snapshot->characters[selected];
    std::array<char, 80> key{};
    const auto length = std::snprintf(key.data(),
                                      key.size(),
                                      "dawning-default-oven-v1-%016llx",
                                      static_cast<unsigned long long>(character.soid));
    if (length <= 0 || static_cast<std::size_t>(length) >= key.size())
        return {DawningOvenBootstrapStatus::refused, false};
    const std::string_view marker(key.data(), static_cast<std::size_t>(length));
    if (store::bootstrap_completed(marker))
        return {transaction.commit() ? DawningOvenBootstrapStatus::ready
                                     : DawningOvenBootstrapStatus::refused,
                false};
    bool held = false;
    for (std::size_t i = 0; i < character.inventory.count; ++i)
        if (character.inventory.values[i].definitionHash == oven.definitionHash) held = true;
    for (const auto& item : character.equipment.slots)
        if (item && item->definitionHash == oven.definitionHash) held = true;
    if (!held) {
        auto pending =
            std::unique_ptr<PendingRecordRewardGrant>{new (std::nothrow) PendingRecordRewardGrant};
        const std::array<DirectRecordReward, 1> reward{{{oven.definitionIndex, 1}}};
        if (!pending
            || !stage_record_reward_grant(*snapshot, reward, kUnclaimedRecordIndex, *pending)
            || !commit_record_reward(*pending))
            return {DawningOvenBootstrapStatus::refused, false};
    }
    if (!store::complete_bootstrap(marker) || !transaction.commit())
        return {DawningOvenBootstrapStatus::refused, false};
    return {DawningOvenBootstrapStatus::ready, !held};
}
} // namespace sunrise::state
