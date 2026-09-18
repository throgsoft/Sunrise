#pragma once

#include <algorithm>

#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/dawning_oven_runtime.h"

namespace sunrise::server::bap::encrypted::push::snapshot::dawning {
namespace account_layout = middleware::datagen::family4::account::layout;
using State = state::account::inventory::dawning::State;

/** Overlay the staged native banks after account encoding, before sealing the server push. */
[[nodiscard]] inline bool project_banks(const State& state,
                                        account_layout::Object& object) noexcept {
    namespace identity = state::account::inventory::dawning;
    if (!std::all_of(
            state.ingredients.begin(), state.ingredients.end(), [](auto v) { return v >= 0; })
        || !std::all_of(state.recipes.begin(), state.recipes.end(), [](auto v) {
               return v == 0 || v == state::unlocks::kFlagSet;
           }))
        return false;
    std::copy(state.ingredients.begin(),
              state.ingredients.end(),
              object.objectiveValues.begin() + identity::kFirstIngredientValue);
    std::copy(state.recipes.begin(),
              state.recipes.end(),
              object.acquiredFlags.begin() + identity::kFirstRecipeFlag);
    return true;
}

/** An oven creates exactly one real profile cookie. Name its encoded serial in the native
 * acquisition ring so the grant is visible through the same observer as other stack rewards. */
[[nodiscard]] inline bool project_socket_result(const state::PendingSocketPlug& mutation,
                                                account_layout::Object& object) noexcept {
    namespace identity = state::account::inventory::dawning;
    if (!mutation.afterDawning) return !mutation.beforeDawning;
    if (!mutation.prepared || !mutation.beforeDawning
        || mutation.targetDefinitionHash != identity::kOvenHash
        || !project_banks(*mutation.afterDawning, object))
        return false;
    if (mutation.socketLane == 4) return true;
    if (mutation.socketLane < 2 || mutation.socketLane > 3 || !mutation.profileChanged
        || mutation.expectedProfileItemCount > mutation.beforeProfileItems.size()
        || mutation.afterProfileItemCount > mutation.afterProfileItems.size()
        || object.profileItemCount != mutation.afterProfileItemCount)
        return false;
    const state::account::inventory::ProfileItem* gain = nullptr;
    std::int32_t greatestSerial = 0;
    for (std::size_t i = 0; i < mutation.expectedProfileItemCount; ++i)
        greatestSerial = (std::max)(greatestSerial, mutation.beforeProfileItems[i].mutationSerial);
    for (std::size_t i = 0; i < mutation.afterProfileItemCount; ++i) {
        const auto& item = mutation.afterProfileItems[i];
        if (!identity::cookie(item.definitionHash) || item.mutationSerial <= greatestSerial)
            continue;
        std::int64_t quantityDelta = 0;
        for (std::size_t j = 0; j < mutation.expectedProfileItemCount; ++j)
            if (mutation.beforeProfileItems[j].definitionHash == item.definitionHash)
                quantityDelta -= mutation.beforeProfileItems[j].quantity;
        for (std::size_t j = 0; j < mutation.afterProfileItemCount; ++j)
            if (mutation.afterProfileItems[j].definitionHash == item.definitionHash)
                quantityDelta += mutation.afterProfileItems[j].quantity;
        // Paying the final Essence can stamp a moved existing cookie too. Only the
        // definition whose balance increased is the baked acquisition.
        if (quantityDelta == 0) continue;
        if (quantityDelta != 1) return false;
        if (gain || item.instanceSoid != 0 || item.quantity <= 0) return false;
        gain = &item;
    }
    if (!gain) return false;
    std::int64_t delta = 0;
    for (std::size_t i = 0; i < mutation.expectedProfileItemCount; ++i)
        if (mutation.beforeProfileItems[i].definitionHash == gain->definitionHash)
            delta -= mutation.beforeProfileItems[i].quantity;
    for (std::size_t i = 0; i < mutation.afterProfileItemCount; ++i)
        if (mutation.afterProfileItems[i].definitionHash == gain->definitionHash)
            delta += mutation.afterProfileItems[i].quantity;
    if (delta != 1) return false;
    state::build_data::items::Definition cookie{};
    if (!state::build_data::find_item_definition_hash(gain->definitionHash, cookie)) return false;
    std::size_t matches = 0;
    for (const auto& item : object.profileItems) {
        if (item.mutationSerial != gain->mutationSerial) continue;
        if (item.definitionIndex != cookie.definitionIndex || item.quantity != gain->quantity
            || item.instanceSoid != 0)
            return false;
        ++matches;
    }
    auto& ring = object.profileInventoryChanges;
    if (matches != 1 || ring.writeSlot != 0 || ring.nextSequence != 0
        || !std::all_of(ring.records.begin(), ring.records.end(), [](const auto& row) {
               return row.sequence == 0 && row.reserved == 0 && row.mutationSerial == 0
                      && row.kind == 0 && row.reservedKind == 0 && row.flags == 0;
           }))
        return false;
    ring.records.front() = {0, 0, gain->mutationSerial, 1, 0, 0};
    ring.writeSlot = ring.nextSequence = 1;
    return true;
}

/** Append existing pursuit residents whose objective tails changed. These are upserts, not
 * acquisitions: they do not add QueueZ resident identities or acquisition-ring entries. */
[[nodiscard]] inline bool append_changed_objectives(
    const state::CharacterState& before,
    const state::CharacterState& after,
    const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
    middleware::datagen::family4::loadout::ResolvedInstances& output) noexcept {
    if (before.inventory.count > before.inventory.values.size()
        || after.inventory.count > after.inventory.values.size()
        || loadout.itemCount > loadout.items.size() || output.itemCount > output.items.size())
        return false;
    for (std::size_t i = 0; i < after.inventory.count; ++i) {
        const auto& held = after.inventory.values[i];
        const state::account::inventory::Item* prior = nullptr;
        for (std::size_t j = 0; j < before.inventory.count; ++j)
            if (before.inventory.values[j].instanceSoid == held.instanceSoid)
                prior = &before.inventory.values[j];
        if (!prior
            || (held.objectiveValues == prior->objectiveValues
                && held.objectiveDefinitionIndex == prior->objectiveDefinitionIndex))
            continue;
        bool present = false;
        for (std::size_t j = 0; j < output.itemCount; ++j)
            present |= output.items[j].instance.instanceSoid == held.instanceSoid;
        if (present) continue;
        bool resolved = false;
        for (std::size_t j = 0; j < loadout.itemCount; ++j) {
            const auto& item = loadout.items[j];
            if (item.instance.instanceSoid != held.instanceSoid) continue;
            if (resolved || item.equipped || output.itemCount == output.items.size()) return false;
            output.items[output.itemCount++] = {item.equipmentSlot, item.instance};
            resolved = true;
        }
        if (!resolved) return false;
    }
    return true;
}
} // namespace sunrise::server::bap::encrypted::push::snapshot::dawning
