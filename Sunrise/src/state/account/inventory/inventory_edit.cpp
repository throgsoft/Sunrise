#include "inventory_edit.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include "../../build_data/runtime.h"
#include "../../investment/store_internal.h"
#include "../account_state.h"

namespace sunrise::state::account::inventory {
namespace {

namespace buckets = build_data::inventory::buckets;
namespace details = build_data::items::details;

/** @return True when the scope names the list the item's installed bucket stores it in. */
[[nodiscard]] bool scope_matches(const QuantityEdit& edit,
                                 buckets::ArraySelector selector) noexcept {
    switch (edit.scope) {
    case EditScope::profile:
        return selector == buckets::ArraySelector::profile;
    case EditScope::characterItem:
        return selector == buckets::ArraySelector::character && edit.instanceSoid != 0;
    case EditScope::characterStack:
        return selector == buckets::ArraySelector::character && edit.instanceSoid == 0;
    }
    return false;
}

/** A replacement must stay a stack of the installed item within its native limit. */
[[nodiscard]] bool replacement_fits(const build_data::items::Definition& definition,
                                    const QuantityEdit& edit,
                                    const char*& reason) noexcept {
    reason = "item_detail";
    details::Definition detail{};
    if (!build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId) {
        return false;
    }
    reason = "quantity";
    return detail.instancedDefinitionState == details::InstancedDefinitionState::stackable
           && edit.quantity <= detail.maxStackSize;
}

/**
 * Picks the serial an edited row receives: one past the greatest profile serial, or the
 * character's next inventory serial.
 * @return False when that list's serials are exhausted.
 */
[[nodiscard]] bool next_serial(const AccountState& saved,
                               const CharacterState& character,
                               EditScope scope,
                               std::int32_t& serial) noexcept {
    constexpr std::int32_t kLastSerial = (std::numeric_limits<std::int32_t>::max)();
    if (scope == EditScope::profile) {
        std::int32_t greatest = 0;
        for (std::size_t index = 0; index < saved.profileItemCount; ++index) {
            greatest = (std::max)(greatest, saved.profileItems[index].mutationSerial);
        }
        if (greatest == kLastSerial) {
            return false;
        }
        serial = greatest + 1;
        return true;
    }
    if (character.nextInventorySerial > static_cast<std::uint32_t>(kLastSerial)) {
        return false;
    }
    serial = static_cast<std::int32_t>(character.nextInventorySerial);
    return true;
}

/**
 * Replaces the expected row's quantity, or removes it and closes the gap when the quantity is
 * zero.
 * @return False when no row carries the expected identity and quantity.
 */
template <typename Row, std::size_t Capacity>
[[nodiscard]] bool edit_row(std::array<Row, Capacity>& rows,
                            std::size_t& count,
                            const QuantityEdit& edit,
                            std::int32_t serial) noexcept {
    for (std::size_t index = 0; index < count && index < Capacity; ++index) {
        Row& row = rows[index];
        if (row.definitionHash != edit.definitionHash || row.mutationSerial != edit.mutationSerial
            || row.quantity != edit.expectedQuantity) {
            continue;
        }
        // Character stacks carry no instance.
        if constexpr (requires { row.instanceSoid; }) {
            if (row.instanceSoid != edit.instanceSoid) {
                continue;
            }
        }
        if (edit.quantity == 0) {
            std::move(rows.begin() + index + 1, rows.begin() + count, rows.begin() + index);
            rows[--count] = {};
        } else {
            row.quantity = edit.quantity;
            row.mutationSerial = serial;
        }
        return true;
    }
    return false;
}

} // namespace

/** Replaces one expected row's quantity and saves the whole account in one savepoint. */
bool set_quantity(const QuantityEdit& edit, QuantityChange& change, const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    reason = "quantity";
    if (edit.quantity < 0) {
        return false;
    }
    reason = "item_definition";
    build_data::items::Definition definition{};
    buckets::Descriptor bucket{};
    if (!build_data::find_item_definition_hash(edit.definitionHash, definition)
        || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)) {
        return false;
    }
    reason = "inventory_scope";
    if (!scope_matches(edit, bucket.arraySelector)) {
        return false;
    }
    if (edit.quantity == 0) {
        reason = "unsupported_removal";
        if (edit.instanceSoid != 0) {
            return false;
        }
    } else if (!replacement_fits(definition, edit, reason)) {
        return false;
    }

    reason = "transaction";
    investment::store::Transaction transaction;
    AccountState saved;
    if (!transaction.ready() || !investment::store::read_account(saved)) {
        return false;
    }
    reason = "account_state";
    if (!account::valid(saved)) {
        return false;
    }
    reason = "character_changed";
    const auto characters = std::span(saved.characters)
                                .first((std::min)(saved.characterCount, saved.characters.size()));
    const auto selected =
        std::find_if(characters.begin(), characters.end(), [](const CharacterState& character) {
            return character.selected;
        });
    if (selected == characters.end() || selected->soid != edit.characterSoid) {
        return false;
    }
    CharacterState& character = *selected;
    reason = "equipped";
    if (edit.instanceSoid != 0
        && std::any_of(character.equipment.slots.begin(),
                       character.equipment.slots.end(),
                       [&edit](const auto& slot) {
                           return slot.has_value() && slot->instanceSoid == edit.instanceSoid;
                       })) {
        return false;
    }
    reason = "account_state";
    std::int32_t serial = 0;
    if (edit.quantity != 0 && !next_serial(saved, character, edit.scope, serial)) {
        return false;
    }

    reason = "inventory_row";
    bool edited = false;
    switch (edit.scope) {
    case EditScope::profile:
        edited = edit_row(saved.profileItems, saved.profileItemCount, edit, serial);
        break;
    case EditScope::characterItem:
        edited = edit_row(character.inventory.values, character.inventory.count, edit, serial);
        break;
    case EditScope::characterStack:
        edited = edit_row(character.stacks.values, character.stacks.count, edit, serial);
        break;
    }
    if (!edited) {
        return false;
    }
    if (edit.scope != EditScope::profile && edit.quantity != 0) {
        ++character.nextInventorySerial;
    }
    reason = "account_state";
    if (!account::valid(saved)) {
        return false;
    }
    reason = "transaction";
    if (!investment::store::write_account(saved) || !transaction.commit()) {
        return false;
    }
    change = {saved.primarySoid,
              character.soid,
              static_cast<std::size_t>(selected - characters.begin()),
              serial};
    reason = nullptr;
    return true;
}

} // namespace sunrise::state::account::inventory
