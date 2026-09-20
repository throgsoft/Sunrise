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
           })) {
        return false;
    }
    std::copy(state.ingredients.begin(),
              state.ingredients.end(),
              object.objectiveValues.begin() + identity::kFirstIngredientValue);
    std::copy(state.recipes.begin(),
              state.recipes.end(),
              object.acquiredFlags.begin() + identity::kFirstRecipeFlag);
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
        || loadout.itemCount > loadout.items.size() || output.itemCount > output.items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < after.inventory.count; ++i) {
        const auto& held = after.inventory.values[i];
        const state::account::inventory::Item* prior = nullptr;
        for (std::size_t j = 0; j < before.inventory.count; ++j) {
            if (before.inventory.values[j].instanceSoid == held.instanceSoid) {
                prior = &before.inventory.values[j];
            }
        }
        if (!prior
            || (held.objectiveValues == prior->objectiveValues
                && held.objectiveDefinitionIndex == prior->objectiveDefinitionIndex)) {
            continue;
        }
        bool present = false;
        for (std::size_t j = 0; j < output.itemCount; ++j) {
            present |= output.items[j].instance.instanceSoid == held.instanceSoid;
        }
        if (present) {
            continue;
        }
        bool resolved = false;
        for (std::size_t j = 0; j < loadout.itemCount; ++j) {
            const auto& item = loadout.items[j];
            if (item.instance.instanceSoid != held.instanceSoid) {
                continue;
            }
            if (resolved || item.equipped || output.itemCount == output.items.size()) {
                return false;
            }
            output.items[output.itemCount++] = {item.equipmentSlot, item.instance};
            resolved = true;
        }
        if (!resolved) {
            return false;
        }
    }
    return true;
}
} // namespace sunrise::server::bap::encrypted::push::snapshot::dawning
