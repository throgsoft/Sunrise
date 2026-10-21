#include "pursuit_snapshot_runtime.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <utility>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"

namespace sunrise::state {
namespace {

namespace pursuits = build_data::pursuits;

/**
 * Copies the account, the selected character's unlock banks and the revision under one store
 * lock, so the inventory and banks come from the same saved state.
 * @param slot Receives the character's slot, which is also its unlock-bank owner.
 */
[[nodiscard]] PursuitSnapshotRefusal read_saved_state(AccountState& account,
                                                      unlocks::Table& values,
                                                      std::size_t& slot,
                                                      std::uint64_t& revision) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    revision = investment::store::g_session.revision;
    account = investment::store::account();
    if (!account::valid(account)) {
        return PursuitSnapshotRefusal::accountUnavailable;
    }
    for (slot = 0; slot < account.characterCount; ++slot) {
        const CharacterState& character = account.characters[slot];
        if (character.selected) {
            break;
        }
    }
    if (slot == account.characterCount) {
        return PursuitSnapshotRefusal::characterUnavailable;
    }
    return investment::store::read_unlocks(values, static_cast<int>(slot))
               ? PursuitSnapshotRefusal::none
               : PursuitSnapshotRefusal::storage;
}

/**
 * Evaluates one inventory resident when it is a pursuit; may throw on allocation.
 * @return False when the resident is not in the pursuit bucket.
 */
[[nodiscard]] bool read_held_pursuit(const account::inventory::Item& item,
                                     const reward_resolution::Context& context,
                                     HeldPursuitStatus& held) {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || definition.bucketId != build_data::items::kPursuitBucketId) {
        return false;
    }
    held.instanceSoid = item.instanceSoid;
    held.definitionHash = item.definitionHash;
    held.quantity = item.quantity;
    held.mutationSerial = item.mutationSerial;
    held.flags = item.flags;
    pursuits::Definition contract{};
    if (!build_data::find_pursuit_definition(definition.definitionIndex, contract)
        || contract.definitionHash != item.definitionHash) {
        return true;
    }
    held.contractFound = true;
    held.lifetimeSeconds = contract.lifetimeSeconds;
    held.completion = inspect_pursuit_completion(contract, context);
    return true;
}

} // namespace

/** Reads one coherent saved view and evaluates each held pursuit's installed contract. */
PursuitSnapshotRefusal read_pursuit_snapshot(PursuitSnapshot& output) noexcept {
    output = {};
    if (build_data::pursuit_catalog_unsupported()) {
        return PursuitSnapshotRefusal::catalogUnsupported;
    }
    if (!build_data::objective_definitions_ready() || !build_data::pursuit_definitions_ready()) {
        return PursuitSnapshotRefusal::catalogUnavailable;
    }
    const std::uint64_t definitionsRevision = build_data::pursuit_definitions_revision();
    AccountState account{};
    unlocks::Table values{};
    std::size_t slot = 0;
    std::uint64_t investmentRevision = 0;
    const PursuitSnapshotRefusal refusal =
        read_saved_state(account, values, slot, investmentRevision);
    if (refusal != PursuitSnapshotRefusal::none) {
        return refusal;
    }
    const CharacterState& character = account.characters[slot];
    // Evaluation never draws, so no seed is needed.
    const reward_resolution::Context context{values, character.characterClass, 0, nullptr};
    try {
        PursuitSnapshot snapshot{};
        snapshot.accountSoid = account.primarySoid;
        snapshot.characterSoid = character.soid;
        snapshot.investmentRevision = investmentRevision;
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            HeldPursuitStatus held{};
            if (read_held_pursuit(character.inventory.values[i], context, held)) {
                snapshot.held.push_back(held);
            }
        }
        // A republish between the contract reads would mix two catalogs in one snapshot.
        if (build_data::pursuit_definitions_revision() != definitionsRevision) {
            return PursuitSnapshotRefusal::catalogUnavailable;
        }
        output = std::move(snapshot);
        return PursuitSnapshotRefusal::none;
    } catch (...) {
        return PursuitSnapshotRefusal::storage;
    }
}

/** Each source is evaluated alone, as a one-instruction program over the same saved copy. */
PursuitSnapshotRefusal read_unlock_values(std::span<const unlocks::Instruction> sources,
                                          std::span<UnlockRead> output,
                                          UnlockReadOrigin& origin) noexcept {
    origin = {};
    std::fill(output.begin(), output.end(), UnlockRead{});
    AccountState account{};
    unlocks::Table values{};
    std::size_t slot = 0;
    std::uint64_t investmentRevision = 0;
    const PursuitSnapshotRefusal refusal =
        read_saved_state(account, values, slot, investmentRevision);
    if (refusal != PursuitSnapshotRefusal::none) {
        return refusal;
    }
    const CharacterState& character = account.characters[slot];
    // Evaluation never draws, so no seed is needed.
    const reward_resolution::Context context{values, character.characterClass, 0, nullptr};
    const std::size_t count = (std::min)(sources.size(), output.size());
    for (std::size_t index = 0; index < count; ++index) {
        UnlockRead& read = output[index];
        read.known = reward_resolution::evaluate(sources.subspan(index, 1), context, read.value);
    }
    origin = {account.primarySoid, character.soid, investmentRevision};
    return PursuitSnapshotRefusal::none;
}

} // namespace sunrise::state
