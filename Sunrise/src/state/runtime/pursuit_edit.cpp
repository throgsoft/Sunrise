#include "pursuit_edit.h"

#include <algorithm>
#include <limits>
#include <span>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_expression.h"
#include "pursuit_completion_runtime.h"

namespace sunrise::state {
namespace {

namespace objectives = build_data::objectives;
namespace pursuits = build_data::pursuits;
namespace store = investment::store;

// The store addresses a row with 16 bits, so a valid operand of every saved bank narrows exactly.
static_assert((std::max)({unlocks::kAccountFlagCapacity,
                          unlocks::kProfileFlagCapacity,
                          unlocks::kCharacterObjectFlagCapacity,
                          unlocks::kObjectiveValueCapacity,
                          unlocks::kCharacterObjectValueCapacity})
              <= (std::numeric_limits<std::uint16_t>::max)());

/** A held pursuit and the installed contract its snapshot is evaluated against. */
struct HeldPursuit {
    /** The selected character's slot, which also owns its unlock banks. */
    std::size_t characterIndex{};
    CharacterClass characterClass{};
    pursuits::Definition contract{};
};

/**
 * Names the saved bank the evaluator reads for one flag or value instruction, as
 * reward_resolver's readers do. Class flags, external values, constants and operators are not
 * saved rows.
 */
[[nodiscard]] bool saved_row(const unlocks::Instruction& instruction, store::Bank& bank) noexcept {
    if (!unlocks::valid(instruction)) {
        return false;
    }
    const bool flag = instruction.opcode == unlocks::Opcode::flag;
    if (!flag && instruction.opcode != unlocks::Opcode::loadValue) {
        return false;
    }
    switch (instruction.bank) {
    case unlocks::Bank::account:
        bank = flag ? store::Bank::accountFlags : store::Bank::objectiveValues;
        return true;
    case unlocks::Bank::profile:
        bank = store::Bank::profileFlags;
        return flag;
    case unlocks::Bank::character:
        bank = flag ? store::Bank::characterObjectFlags : store::Bank::characterObjectValues;
        return true;
    case unlocks::Bank::none:
    case unlocks::Bank::characterClass:
    case unlocks::Bank::external:
        break;
    }
    return false;
}

/** @return True for banks the account object publishes; the rest are the character object's. */
[[nodiscard]] bool account_row(store::Bank bank) noexcept {
    switch (bank) {
    case store::Bank::accountFlags:
    case store::Bank::profileFlags:
    case store::Bank::characterFlags:
    case store::Bank::objectiveValues:
    case store::Bank::accountProgressions:
        return true;
    case store::Bank::characterObjectFlags:
    case store::Bank::characterObjectValues:
    case store::Bank::characterProgressions:
        break;
    }
    return false;
}

/**
 * Finds the identity's pursuit in the account the caller's savepoint reads, and its contract.
 * @param transaction The caller's savepoint, opened before this call.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool read_held_pursuit(const store::Transaction& transaction,
                                     const PursuitIdentity& identity,
                                     AccountState& account,
                                     HeldPursuit& held,
                                     const char*& reason) noexcept {
    reason = "not_ready";
    if (!build_data::item_definitions_ready() || !build_data::objective_definitions_ready()
        || !build_data::pursuit_definitions_ready()) {
        return false;
    }
    reason = "transaction";
    if (!transaction.ready() || !store::read_account(account)) {
        return false;
    }
    reason = "account_state";
    if (!account::valid(account)) {
        return false;
    }
    reason = "character_changed";
    const auto characters =
        std::span(account.characters)
            .first((std::min)(account.characterCount, account.characters.size()));
    const auto selected =
        std::find_if(characters.begin(), characters.end(), [](const CharacterState& character) {
            return character.selected;
        });
    if (selected == characters.end() || selected->soid != identity.characterSoid) {
        return false;
    }
    reason = "pursuit_changed";
    const auto items =
        std::span(selected->inventory.values)
            .first((std::min)(selected->inventory.count, selected->inventory.values.size()));
    const bool copyHeld =
        std::any_of(items.begin(), items.end(), [&identity](const account::inventory::Item& row) {
            return row.instanceSoid == identity.instanceSoid
                   && row.definitionHash == identity.definitionHash
                   && row.mutationSerial == identity.mutationSerial;
        });
    build_data::items::Definition item{};
    if (!copyHeld || !build_data::find_item_definition_hash(identity.definitionHash, item)
        || item.bucketId != build_data::items::kPursuitBucketId) {
        return false;
    }
    reason = "pursuit_contract";
    if (!build_data::find_pursuit_definition(item.definitionIndex, held.contract)
        || !held.contract.readable || held.contract.definitionHash != identity.definitionHash
        || held.contract.objectiveCount > pursuits::kObjectiveCapacity) {
        return false;
    }
    // Without a saved deadline an expired bounty cannot be told apart, so none is edited.
    reason = "expiry_not_saved";
    if (held.contract.lifetimeSeconds > 0) {
        return false;
    }
    held.characterIndex = static_cast<std::size_t>(selected - characters.begin());
    held.characterClass = selected->characterClass;
    return true;
}

/** Evaluates the contract against the selected character's banks as the savepoint holds them. */
[[nodiscard]] bool inspect_saved(const HeldPursuit& held,
                                 unlocks::Table& values,
                                 PursuitCompletionStatus& status) noexcept {
    if (!store::read_unlocks(values, static_cast<int>(held.characterIndex))) {
        return false;
    }
    // Evaluation never draws, so no seed is needed.
    status = inspect_pursuit_completion(held.contract, {values, held.characterClass, 0, nullptr});
    return true;
}

/**
 * Writes one required objective's saved row to its adjusted target when the banks, including
 * earlier writes of this savepoint, leave it incomplete.
 * @param incomplete Set when the objective was incomplete.
 * @param change Counts the row when the write changed it.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool complete_objective(const HeldPursuit& held,
                                      std::size_t ordinal,
                                      unlocks::Table& values,
                                      bool& incomplete,
                                      PursuitEditChange& change,
                                      const char*& reason) noexcept {
    reason = "pursuit_contract";
    objectives::Definition objective{};
    if (!build_data::find_objective_definition(held.contract.objectives[ordinal], objective)) {
        return false;
    }
    // Optional objectives, skipped ones included, never gate completion.
    if (objective.optional) {
        return true;
    }
    reason = "item_progress_not_saved";
    if (objective.itemProgress) {
        return false;
    }
    reason = "transaction";
    PursuitCompletionStatus status{};
    if (!inspect_saved(held, values, status)) {
        return false;
    }
    const PursuitObjectiveStatus& progress = status.objectives[ordinal];
    reason = "objective_source";
    if (!progress.known) {
        return false;
    }
    if (progress.complete) {
        return true;
    }
    incomplete = true;
    reason = "objective_sources";
    if (objective.expression.size() != 1) {
        return false;
    }
    reason = "objective_source";
    const unlocks::Instruction& source = objective.expression.front();
    store::Bank bank{};
    if (!saved_row(source, bank)) {
        return false;
    }
    const std::int32_t after =
        source.opcode == unlocks::Opcode::flag ? unlocks::kFlagSet : progress.target;
    const auto row = static_cast<std::uint16_t>(source.operand);
    reason = "transaction";
    std::int32_t before = 0;
    if (!store::read_unlock(bank, row, before)) {
        return false;
    }
    if (before == after) {
        return true;
    }
    if (!store::write_unlock(bank, row, after)) {
        return false;
    }
    ++change.changedRows;
    change.accountRows = change.accountRows || account_row(bank);
    return true;
}

/**
 * Resolves the one saved row the objective the editor named reads.
 * @param source Receives the objective's only instruction.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool objective_row(const HeldPursuit& held,
                                 std::uint8_t ordinal,
                                 std::uint32_t objectiveHash,
                                 unlocks::Instruction& source,
                                 store::Bank& bank,
                                 const char*& reason) noexcept {
    reason = "objective_changed";
    objectives::Definition objective{};
    if (ordinal >= held.contract.objectiveCount
        || !build_data::find_objective_definition(held.contract.objectives[ordinal], objective)
        || objective.definitionHash != objectiveHash) {
        return false;
    }
    reason = "item_progress_not_saved";
    if (objective.itemProgress) {
        return false;
    }
    reason = "objective_source";
    if (!objective.readable) {
        return false;
    }
    reason = "objective_sources";
    if (objective.expression.size() != 1) {
        return false;
    }
    reason = "objective_source";
    source = objective.expression.front();
    return saved_row(source, bank);
}

/** @return True when every required objective reads complete; optional ones never gate it. */
[[nodiscard]] bool required_objectives_complete(const HeldPursuit& held,
                                                const PursuitCompletionStatus& status) noexcept {
    for (std::size_t ordinal = 0; ordinal < held.contract.objectiveCount; ++ordinal) {
        objectives::Definition objective{};
        if (!build_data::find_objective_definition(held.contract.objectives[ordinal], objective)) {
            return false;
        }
        if (!objective.optional && !status.objectives[ordinal].complete) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Only an objective whose bound program is one saved flag or value row can be set. */
bool set_pursuit_objective(const PursuitIdentity& identity,
                           std::uint8_t ordinal,
                           std::uint32_t objectiveHash,
                           std::int32_t expected,
                           std::int32_t value,
                           PursuitEditChange& change,
                           const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    store::Transaction transaction;
    AccountState account;
    HeldPursuit held{};
    if (!read_held_pursuit(transaction, identity, account, held, reason)) {
        return false;
    }
    unlocks::Instruction source{};
    store::Bank bank{};
    if (!objective_row(held, ordinal, objectiveHash, source, bank, reason)) {
        return false;
    }
    const bool flag = source.opcode == unlocks::Opcode::flag;
    reason = "objective_value";
    if (flag && value != 0 && value != 1) {
        return false;
    }
    reason = "transaction";
    const auto row = static_cast<std::uint16_t>(source.operand);
    std::int32_t before = 0;
    if (!store::read_unlock(bank, row, before)) {
        return false;
    }
    reason = "flag_state";
    if (flag && (before < unlocks::kFlagClear || before > unlocks::kFlagSet)) {
        return false;
    }
    // The evaluator reads a flag as set only at its biased set value.
    const std::int32_t current = flag ? (before == unlocks::kFlagSet ? 1 : 0) : before;
    reason = "value_changed";
    if (current != expected) {
        return false;
    }
    // Every published receipt carries a refresh, so a write that changes nothing is refused.
    reason = "value_unchanged";
    if (current == value) {
        return false;
    }
    reason = "transaction";
    const std::int32_t after =
        flag ? (value != 0 ? unlocks::kFlagSet : unlocks::kFlagClear) : value;
    if (!store::write_unlock(bank, row, after) || !transaction.commit()) {
        return false;
    }
    change = {
        account.primarySoid, identity.characterSoid, held.characterIndex, account_row(bank), 1};
    reason = nullptr;
    return true;
}

/** Writes in contract order, so each objective is evaluated against the rows already written. */
bool complete_pursuit_objectives(const PursuitIdentity& identity,
                                 PursuitEditChange& change,
                                 const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    store::Transaction transaction;
    AccountState account;
    HeldPursuit held{};
    if (!read_held_pursuit(transaction, identity, account, held, reason)) {
        return false;
    }
    unlocks::Table values;
    PursuitEditChange edited{account.primarySoid, identity.characterSoid, held.characterIndex};
    bool incomplete = false;
    for (std::size_t ordinal = 0; ordinal < held.contract.objectiveCount; ++ordinal) {
        if (!complete_objective(held, ordinal, values, incomplete, edited, reason)) {
            return false;
        }
    }
    reason = "objectives_complete";
    if (!incomplete) {
        return false;
    }
    // A zero target, a flag target other than one, an adjustment or a shared row the writes moved
    // can still leave an objective incomplete; the savepoint then rolls every write back.
    reason = "transaction";
    PursuitCompletionStatus status{};
    if (!inspect_saved(held, values, status)) {
        return false;
    }
    reason = "objective_target";
    if (!required_objectives_complete(held, status)) {
        return false;
    }
    reason = "transaction";
    if (!transaction.commit()) {
        return false;
    }
    change = edited;
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
