#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;

/** Prepares one selected-character unequipped item removal without changing account State. */
bool prepare_item_dismantle(std::uint64_t instanceSoid, PendingItemDismantle& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (instanceSoid == 0 || !account::valid(account)) {
        report_dismantle("prepare", "fail", "input", 0, 0, instanceSoid, 0, 0, 0, 0, 0);
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= account.characterCount
        || !stage_item_dismantle(account, characterIndex, instanceSoid, true, mutation)) {
        report_dismantle(
            "prepare", "fail", "ownership_or_resolve", 0, 0, instanceSoid, 0, 0, 0, 0, 0);
        return false;
    }

    report_dismantle("prepare",
                     "ok",
                     "ready",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     mutation.dismantledInstanceSoid,
                     mutation.inventoryIndex,
                     mutation.inventoryRow,
                     mutation.equipmentSlot,
                     mutation.movedInventoryItemCount,
                     mutation.afterCharacter.nextInventorySerial);
    return true;
}

/** Prepares one exact expected unequipped instance's removal, crediting no dismantle materials. */
bool prepare_item_removal(std::uint64_t characterSoid,
                          std::uint64_t instanceSoid,
                          std::uint32_t definitionHash,
                          std::int32_t mutationSerial,
                          PendingItemDismantle& mutation,
                          const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    mutation = {};
    reason = "account_state";
    const AccountState account = account_snapshot();
    if (!account::valid(account)) {
        return false;
    }
    reason = "character_changed";
    const std::size_t characterIndex = selected_character_index(account);
    if (characterIndex >= account.characterCount
        || account.characters[characterIndex].soid != characterSoid) {
        return false;
    }
    const CharacterState& character = account.characters[characterIndex];
    CharacterItemLocation location{};
    reason = "inventory_row";
    if (instanceSoid == 0 || !find_character_item_location(character, instanceSoid, location)) {
        return false;
    }
    reason = "equipped";
    if (location.equipped) {
        return false;
    }
    reason = "inventory_row";
    const account::inventory::Item* item = character_item_at(character, location);
    if (item == nullptr || item->definitionHash != definitionHash
        || item->mutationSerial != mutationSerial) {
        return false;
    }
    // Locked items and rows the loadout cannot place stay with the in-game dismantle rules.
    reason = "unsupported_removal";
    if (!stage_item_dismantle(account, characterIndex, instanceSoid, false, mutation)) {
        report_dismantle("prepare_removal",
                         "fail",
                         reason,
                         definitionHash,
                         characterSoid,
                         instanceSoid,
                         0,
                         0,
                         0,
                         0,
                         0);
        return false;
    }
    report_dismantle("prepare_removal",
                     "ok",
                     "ready",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     mutation.dismantledInstanceSoid,
                     mutation.inventoryIndex,
                     mutation.inventoryRow,
                     mutation.equipmentSlot,
                     mutation.movedInventoryItemCount,
                     mutation.afterCharacter.nextInventorySerial);
    reason = nullptr;
    return true;
}

/** Produces the exact character-and-profile after-image while the captured views remain current. */
bool preview_item_dismantle(const PendingItemDismantle& mutation, AccountState& after) noexcept {
    const AccountState current = account_snapshot();
    const bool ready = materialize_item_dismantle(current, mutation, after);
    report_dismantle("preview",
                     ready ? "ok" : "fail",
                     ready ? "ready" : "stale_or_invalid",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     mutation.dismantledInstanceSoid,
                     mutation.inventoryIndex,
                     mutation.inventoryRow,
                     mutation.equipmentSlot,
                     mutation.movedInventoryItemCount,
                     mutation.afterCharacter.nextInventorySerial);
    return ready;
}

/** Commits one prepared dismantle only while its complete account views are unchanged. */
bool commit_item_dismantle(PendingItemDismantle& mutation) noexcept {
    const PendingItemDismantle& prepared = mutation;
    const PendingConsumption consume{mutation};
    const auto fail = [&prepared](std::string_view reason) noexcept {
        report_dismantle("commit",
                         "fail",
                         reason,
                         prepared.dismantledItem.definitionHash,
                         prepared.characterSoid,
                         prepared.dismantledInstanceSoid,
                         prepared.inventoryIndex,
                         prepared.inventoryRow,
                         prepared.equipmentSlot,
                         prepared.movedInventoryItemCount,
                         prepared.afterCharacter.nextInventorySerial);
        return false;
    };

    report_dismantle("commit_begin",
                     "ok",
                     "ready",
                     prepared.dismantledItem.definitionHash,
                     prepared.characterSoid,
                     prepared.dismantledInstanceSoid,
                     prepared.inventoryIndex,
                     prepared.inventoryRow,
                     prepared.equipmentSlot,
                     prepared.movedInventoryItemCount,
                     prepared.afterCharacter.nextInventorySerial);

    investment::store::g_mutex.lock();
    AccountState candidate{};
    const bool ready =
        materialize_item_dismantle(investment::store::account(), prepared, candidate);
    if (ready) {
        if (!investment::store::write_account(candidate)) {
            investment::store::g_mutex.unlock();
            return false;
        }
    }
    investment::store::g_mutex.unlock();

    if (!ready) {
        return fail("stale_or_invalid");
    }

    report_dismantle("commit_end",
                     "ok",
                     "published",
                     prepared.dismantledItem.definitionHash,
                     prepared.characterSoid,
                     prepared.dismantledInstanceSoid,
                     prepared.inventoryIndex,
                     prepared.inventoryRow,
                     prepared.equipmentSlot,
                     prepared.movedInventoryItemCount,
                     prepared.afterCharacter.nextInventorySerial);
    return true;
}

} // namespace sunrise::state
