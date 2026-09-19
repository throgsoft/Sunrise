#include "dawning_pickup_release.h"

#include <Windows.h>

#include <algorithm>
#include <memory>
#include <new>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/investment/store_internal.h"
#include "../../../../state/runtime/dawning_reward_runtime.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/internal.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {
bool append_pickups(Session& session,
                    Scratch& scratch,
                    std::array<std::byte, state::kBapNonceSize>& nonce,
                    std::size_t& size,
                    queuez::SessionState& after) noexcept {
    namespace snap = push::snapshot;
    namespace character = middleware::datagen::family4::character;
    auto account = std::unique_ptr<state::AccountState>{new (std::nothrow) state::AccountState};
    if (!account || !state::investment::store::read_account(*account)) {
        return false;
    }
    const auto selectedIndex = snap::find_character_index(*account);
    snap::Resolved resolved{};
    queuez::EquipmentSwap update{};
    if (!selectedIndex || !snap::resolve(*account, *selectedIndex, resolved)
        || account->primarySoid != session.queuez.family4RootSoid
        || !queuez::stage_equipment_swap(
            session.queuez, account->characters[*selectedIndex].soid, update)) {
        return false;
    }
    const auto bytes = std::span(scratch.plaintext).first(character::layout::kObjectSize);
    if (!character::encode(account->characters[*selectedIndex],
                           resolved.loadout,
                           resolved.lightEvaluation,
                           bytes)) {
        return false;
    }
    auto& object = *reinterpret_cast<character::layout::Object*>(bytes.data());
    namespace identity = state::account::inventory::dawning;
    std::size_t count = 0;
    for (const auto& row : object.inventoryItems) {
        state::build_data::items::Definition item{};
        if (row.definitionIndex == 0xFFFF) {
            continue;
        }
        if (!state::build_data::find_item_definition_index(row.definitionIndex, item)) {
            return false;
        }
        const auto ingredient = identity::ingredient(item.definitionHash);
        if (ingredient == identity::kIngredientCount
            || item.definitionHash != identity::kIngredients[ingredient].pickupHash) {
            continue;
        }
        if (row.instanceSoid != 0 || row.quantity != 1
            || count == object.inventoryChanges.records.size()) {
            return false;
        }
        auto& change = object.inventoryChanges.records[count];
        change.sequence = static_cast<std::uint16_t>(count++);
        change.mutationSerial = row.mutationSerial;
        change.kind = snap::kChangeKind;
        change.flags = snap::kChangeFlags;
    }
    if (count == 0) {
        return false;
    }
    object.inventoryChanges.writeSlot = object.inventoryChanges.nextSequence =
        static_cast<std::uint16_t>(count);
    snap::Prepared prepared{};
    std::size_t extent{};
    if (!snap::append_object(scratch,
                             bytes,
                             resolved.characterObjectId,
                             account->characters[*selectedIndex].soid,
                             prepared.objects[0],
                             extent)) {
        return false;
    }
    prepared.rawClearSize = bytes.size();
    prepared.compressedClearSize = extent;
    prepared.family = {queuez::kAccountFamilyType,
                       account->primarySoid,
                       update.after.family4Version,
                       0,
                       std::span(prepared.objects).first(1)};
    if (!push::queuez_frame::append_prepared_frame(
            scratch, prepared, session.sessionKey, nonce, scratch.framed, size)) {
        return false;
    }
    after = update.after;
    return true;
}
} // namespace

bool consume_dawning_pickup_release(Session& session,
                                    Scratch& scratch,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool& touchesScratch) noexcept {
    const auto now = GetTickCount64();
    if (!session.queuez.family4Active || session.queuez.family4Version == 0
        || now < session.dawningPickupSweepDueTick) {
        return false;
    }
    // One revision drops the rows it replaces and carries their successors, so no observer
    // grace separates them. The due tick is a backoff for a sweep that could not proceed.
    session.dawningPickupSweepDueTick = now + 1'000;
    state::investment::store::Transaction transaction;
    auto account = std::unique_ptr<state::AccountState>{new (std::nothrow) state::AccountState};
    if (!transaction.ready() || !account || !state::investment::store::read_account(*account)
        || account->primarySoid != session.queuez.family4RootSoid) {
        return false;
    }
    const state::CharacterState* selected = nullptr;
    for (std::size_t i = 0; i < account->characterCount; ++i) {
        if (account->characters[i].selected) {
            selected = &account->characters[i];
        }
    }
    if (!selected) {
        return false;
    }
    std::size_t acquired{};
    if (!state::runtime::detail::dawning::stage_queued_pickups(selected->soid, acquired)
        || acquired == 0) {
        return false;
    }

    touchesScratch = true;
    auto nonce = session.sendNonce;
    queuez::SessionState after{};
    std::size_t size{};
    // The outer transaction keeps the deletion private until the complete frame fits.
    // Failure rolls back the rows and leaves the oven counters untouched in either case.
    const bool encoded = append_pickups(session, scratch, nonce, size, after);
    if (!encoded || size == 0 || size > response.size() || !queuez::valid(after)
        || !transaction.commit()) {
        return false;
    }
    std::copy_n(scratch.framed.begin(), size, response.begin());
    written = size;
    session.sendNonce = nonce;
    session.queuez = after;
    // The next batch may publish on the very next pump: its revision drops these rows itself.
    session.dawningPickupSweepDueTick = now;
    bap::arm_acquisition_presentation_hold(session);
    bap::arm_account_resync_elsewhere(session);
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=dawning_pickup stage=published revision=%d rows=%zu "
                      "oven_balance=preserved",
                      after.family4Version,
                      acquired);
    return true;
}
} // namespace sunrise::server::bap::encrypted
