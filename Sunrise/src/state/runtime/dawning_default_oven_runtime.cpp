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
    if (snapshot->characterCount == 0) return {DawningOvenBootstrapStatus::notReady, false};
    const auto originallySelected = selected_character_index(*snapshot);
    bool changed = false;
    // The initial Family-4 snapshot precedes character selection. Seed every character here,
    // otherwise a later preflight can persist an oven without publishing its new item object.
    for (std::size_t characterIndex = 0; characterIndex < snapshot->characterCount;
         ++characterIndex) {
        auto& character = snapshot->characters[characterIndex];
        std::array<char, 80> key{};
        const auto length = std::snprintf(key.data(),
                                          key.size(),
                                          "dawning-default-oven-v1-%016llx",
                                          static_cast<unsigned long long>(character.soid));
        if (length <= 0 || static_cast<std::size_t>(length) >= key.size())
            return {DawningOvenBootstrapStatus::refused, false};
        const std::string_view marker(key.data(), static_cast<std::size_t>(length));
        if (store::bootstrap_completed(marker)) continue;
        bool held = false;
        for (std::size_t i = 0; i < character.inventory.count; ++i)
            if (character.inventory.values[i].definitionHash == oven.definitionHash) held = true;
        for (const auto& item : character.equipment.slots)
            if (item && item->definitionHash == oven.definitionHash) held = true;
        if (!held) {
            auto pending =
                std::unique_ptr<PendingItemAcquisition>{new (std::nothrow) PendingItemAcquisition};
            // Select only in this private staging view, so the common acquisition validator can
            // resolve the target character. Never publish a synthetic character selection.
            for (std::size_t i = 0; i < snapshot->characterCount; ++i)
                snapshot->characters[i].selected = i == characterIndex;
            if (!pending
                || !finalize_item_acquisition(
                    *snapshot, *snapshot, oven.definitionHash, false, {.direct = true}, *pending))
                return {DawningOvenBootstrapStatus::refused, false};
            character = pending->afterCharacter;
            for (std::size_t i = 0; i < snapshot->characterCount; ++i)
                snapshot->characters[i].selected = i == originallySelected;
            changed = true;
        }
        if (!store::complete_bootstrap(marker)) return {DawningOvenBootstrapStatus::refused, false};
    }
    if ((changed && !store::write_account(*snapshot)) || !transaction.commit())
        return {DawningOvenBootstrapStatus::refused, false};
    return {DawningOvenBootstrapStatus::ready, changed};
}
} // namespace sunrise::state
