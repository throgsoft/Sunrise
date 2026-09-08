#pragma once

#include <span>

#include "../../build_data/runtime.h"
#include "../account_state.h"
#include "dawning_oven_state.h"

namespace sunrise::state::account::inventory::dawning {

/** Retained native chooser condition VAL(12496) maps to account objective-value row 5627. */
inline constexpr std::size_t kChooserReadinessAccountValue = 5627;

/** Sunrise default-oven policy retained from the working server projection. An owned tutorial
 * or an existing nonzero value keeps its actual state. This changes only the encoded copy:
 * no tutorial completion, ingredient balance, recipe flag or inventory resident is fabricated. */
[[nodiscard]] inline bool project_chooser_readiness(const AccountState& account,
                                                    std::span<std::int32_t> values) noexcept {
    if (!account::valid(account)) return false;
    bool heldOven = false, heldTutorial = false;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            const auto& item = character.inventory.values[i];
            if (item.instanceSoid == 0 || item.quantity <= 0) continue;
            heldOven |= item.definitionHash == kOvenHash && item.quantity == 1;
            switch (item.definitionHash) {
            case 0x9282075CU:
            case 0x2489A937U:
            case 0x9E86974EU:
            case 0x207A9801U:
                heldTutorial = true;
                break;
            default:
                break;
            }
        }
    }
    if (!heldOven || heldTutorial) return true;
    // An unowned optional oven cannot prevent the initial account snapshot from being sent.
    build_data::items::Definition oven{};
    build_data::items::details::Definition detail{};
    if (!build_data::find_item_definition_hash(kOvenHash, oven)
        || !build_data::find_configured_item_detail(oven.definitionIndex, detail)
        || detail.definitionHash != kOvenHash || detail.definitionIndex != oven.definitionIndex
        || detail.bucketId != oven.bucketId || detail.ordinarySocketCount != 5
        || detail.ordinarySocketState != build_data::items::details::OrdinarySocketState::present)
        return false;
    if (values.size() <= kChooserReadinessAccountValue) return false;
    if (values[kChooserReadinessAccountValue] == 0) values[kChooserReadinessAccountValue] = -1;
    return true;
}
} // namespace sunrise::state::account::inventory::dawning
