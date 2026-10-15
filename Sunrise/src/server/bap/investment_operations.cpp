#include "investment_operations.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "../../core/logging/log.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "../../state/runtime/season_pass_snapshot_runtime.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** The request type an operation kind names. */
template <InvestmentOperationKind kind>
using KindRequest = std::variant_alternative_t<static_cast<std::size_t>(kind), InvestmentRequest>;
static_assert(std::variant_size_v<InvestmentRequest>
              == static_cast<std::size_t>(InvestmentOperationKind::resetSeasonPassClaim) + 1);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::grantItem>, GrantItemOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::setQuantity>,
                             state::account::inventory::QuantityEdit>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::setSeasonalExperience>,
                             SeasonalExperienceOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::claimSeasonPassReward>,
                             SeasonPassClaimOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::resetSeasonPassClaim>,
                             SeasonPassResetOperation>);

/** One submitted operation and where it stands. */
struct RetainedOperation {
    InvestmentOperation operation{};
    InvestmentReceipt receipt{};
};

/**
 * Every receipt the editor has not acknowledged, oldest first. Guarded by the session lock.
 * Each entry is one explicit editor action, and the editor acknowledges a receipt once it reads it
 * finished, so the table holds no count of its own; only the allocator refuses an entry.
 */
std::vector<RetainedOperation> g_operations{};

/** @return True once the receipt can no longer change. */
[[nodiscard]] bool finished(InvestmentReceiptState state) noexcept {
    return state == InvestmentReceiptState::refused || state == InvestmentReceiptState::published
           || state == InvestmentReceiptState::committedUnpublished;
}

/** @return The retained operation, or null. Caller owns the session lock. */
[[nodiscard]] RetainedOperation* find_operation(std::uint64_t operationId) noexcept {
    const auto found =
        std::find_if(g_operations.begin(), g_operations.end(), [&](const RetainedOperation& entry) {
            return entry.operation.id == operationId;
        });
    return found == g_operations.end() ? nullptr : &*found;
}

/** @return The authenticated Family-4 peer rooted at the account, or null. Caller owns the lock. */
[[nodiscard]] const Session* publication_peer(std::uint64_t accountSoid) noexcept {
    for (const Session& session : sessions()) {
        if (session.id != 0 && session.authenticated && session.queuez.family4Active
            && session.queuez.family4RootSoid == accountSoid) {
            return &session;
        }
    }
    return nullptr;
}

/**
 * Copies a receipt, naming what a pending operation waits on now: no peer of its account can
 * run it, or that peer must resync first. Caller owns the session lock.
 */
[[nodiscard]] InvestmentReceipt present(const RetainedOperation& retained) noexcept {
    InvestmentReceipt receipt = retained.receipt;
    if (finished(receipt.state)) {
        return receipt;
    }
    const Session* peer = publication_peer(receipt.target.accountSoid);
    if (peer == nullptr) {
        receipt.state = InvestmentReceiptState::deferred;
        receipt.wait = InvestmentWait::publicationPeer;
    } else if (peer->accountResyncArmed) {
        receipt.state = InvestmentReceiptState::deferred;
        receipt.wait = InvestmentWait::accountResync;
    }
    return receipt;
}

/** Moves a receipt, stamping when it finished. */
void apply_transition(InvestmentReceipt& receipt, const InvestmentTransition& transition) noexcept {
    receipt.state = transition.state;
    receipt.wait = transition.wait;
    receipt.reason = transition.reason != nullptr ? transition.reason : "";
    receipt.family4Version = transition.family4Version;
    receipt.changedRows = transition.changedRows;
    receipt.finishedTick = finished(transition.state) ? GetTickCount64() : 0;
}

/** Logs one receipt as it stands after a stage of its life. */
void report_receipt(const char* stage, const InvestmentReceipt& receipt) noexcept {
    core::log::writef(core::log::Channel::server,
                      receipt.state == InvestmentReceiptState::committedUnpublished
                          ? core::log::Level::warn
                          : core::log::Level::info,
                      "ev=studio_operation stage=%s id=%llu kind=%u state=%u wait=%u reason=%s "
                      "version=%d rows=%u",
                      stage,
                      static_cast<unsigned long long>(receipt.operationId),
                      static_cast<unsigned>(receipt.kind),
                      static_cast<unsigned>(receipt.state),
                      static_cast<unsigned>(receipt.wait),
                      receipt.reason,
                      static_cast<int>(receipt.family4Version),
                      static_cast<unsigned>(receipt.changedRows));
}

/** @return True while the item tables an inventory grant or edit resolves through still load. */
[[nodiscard]] bool inventory_catalogs_loading() noexcept {
    return !state::build_data::item_definitions_ready()
           || !state::build_data::configured_item_details_ready()
           || !state::build_data::inventory_bucket_descriptors_ready();
}

/** @return Queued while the live account still is the one the editor read, else a refusal. */
[[nodiscard]] InvestmentTransition check_target(const InvestmentTarget& target) noexcept {
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        return refused_transition("account_state");
    }
    if (account.primarySoid != target.accountSoid) {
        return refused_transition("account_changed");
    }
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (selected == 0 || selected != target.characterSoid) {
        return refused_transition("character_changed");
    }
    if (target.contentHash != state::build_data::content_hash()) {
        return refused_transition("catalog_changed");
    }
    return {};
}

/** A grant names an installed item, a positive quantity and an inventory State can fill. */
[[nodiscard]] InvestmentTransition check_request(const GrantItemOperation& request,
                                                 const InvestmentTarget& /*target*/) noexcept {
    if (inventory_catalogs_loading()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    state::build_data::items::Definition item{};
    if (!state::build_data::find_item_definition_hash(request.definitionHash, item)) {
        return refused_transition("item_definition");
    }
    if (request.quantity == 0
        || request.quantity
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return refused_transition("quantity");
    }
    switch (state::item_grant_route(item.definitionIndex)) {
    case state::ItemGrantRoute::unavailable:
        return refused_transition("item_route");
    case state::ItemGrantRoute::quest:
        // A quest item is one instance; its acquisition takes no quantity.
        return request.quantity == 1 ? InvestmentTransition{}
                                     : refused_transition("instance_quantity");
    case state::ItemGrantRoute::profile:
    case state::ItemGrantRoute::reward:
        break;
    }
    return {};
}

/** An edit names the selected character, an installed item and a quantity State can hold. */
[[nodiscard]] InvestmentTransition
check_request(const state::account::inventory::QuantityEdit& edit,
              const InvestmentTarget& target) noexcept {
    if (inventory_catalogs_loading()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    if (edit.characterSoid != target.characterSoid) {
        return refused_transition("character_changed");
    }
    if (edit.quantity < 0) {
        return refused_transition("quantity");
    }
    state::build_data::items::Definition item{};
    if (!state::build_data::find_item_definition_hash(edit.definitionHash, item)) {
        return refused_transition("item_definition");
    }
    return {};
}

/** State compares the expected total when the replacement runs; only its range is static. */
[[nodiscard]] InvestmentTransition check_request(const SeasonalExperienceOperation& request,
                                                 const InvestmentTarget& /*target*/) noexcept {
    return request.replacement < 0 ? refused_transition("experience_range")
                                   : InvestmentTransition{};
}

/** A claim passes every opcode-2400 rule that needs no draw. */
[[nodiscard]] InvestmentTransition check_request(const SeasonPassClaimOperation& request,
                                                 const InvestmentTarget& /*target*/) noexcept {
    const char* reason = nullptr;
    if (state::season_pass_reward_claimable(request.rewardIndex, request.itemHash, &reason)) {
        return {};
    }
    return state_refusal_transition(reason);
}

/** A reset names an installed pass row by its index and the item it grants. */
[[nodiscard]] InvestmentTransition check_request(const SeasonPassResetOperation& request,
                                                 const InvestmentTarget& /*target*/) noexcept {
    if (!state::build_data::season_pass_ready()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    state::build_data::season_pass::Reward reward{};
    if (!state::build_data::find_season_pass_reward(request.rewardIndex, reward)) {
        return refused_transition("reward_index");
    }
    if (reward.itemHash != request.itemHash) {
        return refused_transition("reward_hash");
    }
    return {};
}

/** @return The operation's request checked against its kind's rules. */
[[nodiscard]] InvestmentTransition
check_operation_request(const InvestmentOperation& operation) noexcept {
    return std::visit(
        [&](const auto& request) noexcept { return check_request(request, operation.target); },
        operation.request);
}

/** @return Queued when a submission may wait for its peer's poll; the peer follows the target. */
[[nodiscard]] InvestmentTransition check_submission(const InvestmentOperation& operation) noexcept {
    const InvestmentTransition verdict = check_target(operation.target);
    if (verdict.state != InvestmentReceiptState::queued) {
        return verdict;
    }
    // Every change publishes through a peer of the account, so none commits without one.
    if (publication_peer(operation.target.accountSoid) == nullptr) {
        return refused_transition("publication_peer");
    }
    return check_operation_request(operation);
}

/** Retains one entry; only a failed allocation refuses it, and nothing is then retained. */
[[nodiscard]] bool retain_operation(const RetainedOperation& entry) noexcept {
    try {
        g_operations.push_back(entry);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

} // namespace

/** The request alternatives follow the kinds, so the alternative's position is the kind. */
InvestmentOperationKind investment_operation_kind(const InvestmentOperation& operation) noexcept {
    return static_cast<InvestmentOperationKind>(operation.request.index());
}

/** State names `not_ready` only while a catalog it needs is still unpublished. */
InvestmentTransition state_refusal_transition(const char* reason) noexcept {
    if (reason == nullptr) {
        return refused_transition("transaction");
    }
    return std::string_view(reason) == "not_ready" ? deferred_transition(InvestmentWait::catalog)
                                                   : refused_transition(reason);
}

/** The target is checked before the request, so an editor bound to stale context learns that. */
InvestmentTransition check_investment_operation(const InvestmentOperation& operation) noexcept {
    const InvestmentTransition verdict = check_target(operation.target);
    if (verdict.state != InvestmentReceiptState::queued) {
        return verdict;
    }
    return check_operation_request(operation);
}

/** Operations run in submission order per account, so a dependent edit reads its predecessor. */
bool next_investment_operation_locked(std::uint64_t accountSoid,
                                      InvestmentOperation& operation) noexcept {
    for (const RetainedOperation& entry : g_operations) {
        if (!finished(entry.receipt.state) && entry.operation.target.accountSoid == accountSoid) {
            operation = entry.operation;
            return true;
        }
    }
    return false;
}

/** Logs only a change, so an operation waiting across polls logs once. */
void record_investment_transition_locked(std::uint64_t operationId,
                                         const InvestmentTransition& transition) noexcept {
    RetainedOperation* retained = find_operation(operationId);
    if (retained == nullptr || finished(retained->receipt.state)) {
        return;
    }
    const bool changed =
        retained->receipt.state != transition.state || retained->receipt.wait != transition.wait;
    apply_transition(retained->receipt, transition);
    if (changed) {
        report_receipt("execute", retained->receipt);
    }
}

/** Receipts are process-local; nothing survives a game exit. */
void release_investment_operations_locked() noexcept {
    std::vector<RetainedOperation>{}.swap(g_operations);
}

/** Reads the account and its revision together, then the peers bound to it. */
bool investment_context(InvestmentContext& output) noexcept {
    output = {};
    const std::shared_lock lock(session_lock());
    std::uint64_t revision = 0;
    const state::AccountState account = state::account_snapshot(revision);
    if (!state::account::valid(account)) {
        return false;
    }
    output.live = {account.primarySoid,
                   state::account::selected_character_soid(account),
                   state::build_data::content_hash()};
    output.characterCount = (std::min)(account.characterCount, output.characters.size());
    for (std::size_t index = 0; index < output.characterCount; ++index) {
        const state::CharacterState& character = account.characters[index];
        output.characters[index] = {character.soid, character.characterClass, character.selected};
    }
    output.revision = revision;
    output.peerPresent = has_active_family4_peer();
    const Session* peer = publication_peer(account.primarySoid);
    output.peerOwnsAccount = peer != nullptr;
    output.peerResyncArmed = peer != nullptr && peer->accountResyncArmed;
    return true;
}

/** A duplicate identity never runs again: it answers with the receipt already retained. */
InvestmentSubmission submit_investment_operation(const InvestmentOperation& operation,
                                                 InvestmentReceipt& receipt) noexcept {
    const std::lock_guard lock(session_lock());
    if (const RetainedOperation* retained = find_operation(operation.id)) {
        receipt = present(*retained);
        return retained->operation == operation ? InvestmentSubmission::existing
                                                : InvestmentSubmission::reused;
    }
    RetainedOperation entry{};
    entry.operation = operation;
    entry.receipt.operationId = operation.id;
    entry.receipt.target = operation.target;
    entry.receipt.kind = investment_operation_kind(operation);
    entry.receipt.submittedTick = GetTickCount64();
    apply_transition(entry.receipt, check_submission(operation));
    if (!retain_operation(entry)) {
        apply_transition(entry.receipt, refused_transition("allocation"));
        report_receipt("submit", entry.receipt);
        receipt = entry.receipt;
        return InvestmentSubmission::refused;
    }
    report_receipt("submit", entry.receipt);
    receipt = present(entry);
    return InvestmentSubmission::accepted;
}

/** Answers from the retained receipt alone; nothing runs. */
bool investment_receipt(std::uint64_t operationId, InvestmentReceipt& receipt) noexcept {
    const std::shared_lock lock(session_lock());
    const RetainedOperation* retained = find_operation(operationId);
    receipt = retained != nullptr ? present(*retained) : InvestmentReceipt{};
    return retained != nullptr;
}

/** A pending receipt stays, so the editor can still learn how its operation ends. */
bool acknowledge_investment_receipt(std::uint64_t operationId,
                                    InvestmentReceipt& receipt) noexcept {
    const std::lock_guard lock(session_lock());
    RetainedOperation* retained = find_operation(operationId);
    if (retained == nullptr) {
        receipt = {};
        return false;
    }
    receipt = present(*retained);
    if (!finished(retained->receipt.state)) {
        return false;
    }
    report_receipt("acknowledge", retained->receipt);
    g_operations.erase(g_operations.begin() + (retained - g_operations.data()));
    return true;
}

/** The lane runs under the same lock, so a withdrawal never races a commit. */
bool withdraw_investment_operation(std::uint64_t operationId, InvestmentReceipt& receipt) noexcept {
    const std::lock_guard lock(session_lock());
    RetainedOperation* retained = find_operation(operationId);
    if (retained == nullptr) {
        receipt = {};
        return false;
    }
    if (!finished(retained->receipt.state)) {
        apply_transition(retained->receipt, refused_transition("withdrawn"));
        report_receipt("withdraw", retained->receipt);
    }
    receipt = present(*retained);
    return true;
}

} // namespace sunrise::server::bap
