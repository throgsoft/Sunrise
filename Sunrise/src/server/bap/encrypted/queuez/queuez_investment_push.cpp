#include "queuez_investment_push.h"

#include <Windows.h>

#include <memory>
#include <new>
#include <span>
#include <utility>
#include <variant>

#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/inventory/inventory_edit.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/investment/store_internal.h"
#include "../../../../state/runtime/pursuit_edit.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../web_service/web_service_actions.h"
#include "../transactions/service_outcome_commit.h"
#include "queuez_item_grant_push.h"
#include "queuez_reward_staging.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

using state::account::inventory::EditScope;
using state::account::inventory::QuantityEdit;

/** A quantity edit, a removal, an XP total and a pass flag each change one saved row. */
constexpr std::uint32_t kEditedRowCount = 1;

/** The objects one edit changed: the account and selected character, or that character alone. */
struct ChangedObjects {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    /** The selected character's slot, read only when the account object is unchanged. */
    std::size_t characterIndex{};
    bool account{};
};

/** @return A published transition for the frame the peer now holds. */
[[nodiscard]] InvestmentTransition published_transition(const StagedFrame& frame,
                                                        std::uint32_t changedRows) noexcept {
    return {.state = InvestmentReceiptState::published,
            .family4Version = frame.after.family4Version,
            .changedRows = changedRows};
}

/**
 * Encodes the changed objects from State the caller's savepoint still holds uncommitted.
 * @param reason Receives inventory_stage or inventory_encode on failure.
 */
[[nodiscard]] bool stage_changed_objects(const ChangedObjects& changed,
                                         Session& session,
                                         Scratch& scratch,
                                         StagedFrame& frame,
                                         const char*& reason) noexcept {
    queuez::EquipmentSwap update{};
    if (!queuez::stage_equipment_swap(session.queuez, changed.characterSoid, update)) {
        reason = "inventory_stage";
        return false;
    }
    frame.sendNonce = session.sendNonce;
    const auto presentationRows = active_acquisition_presentation_rows(session);
    const bool encoded =
        changed.account
            ? push::append_account_refresh_notification(scratch,
                                                        update,
                                                        presentationRows,
                                                        session.sessionKey,
                                                        frame.sendNonce,
                                                        scratch.framed,
                                                        frame.size)
            : push::append_selected_character_refresh_notification(scratch,
                                                                   update,
                                                                   changed.accountSoid,
                                                                   changed.characterIndex,
                                                                   presentationRows,
                                                                   session.sessionKey,
                                                                   frame.sendNonce,
                                                                   scratch.framed,
                                                                   frame.size);
    if (!encoded || frame.size == 0) {
        reason = "inventory_encode";
        return false;
    }
    middleware::secure_channel::advance_nonce(frame.sendNonce);
    frame.after = update.after;
    return true;
}

/** Commits the operation's savepoint and copies its frame out, or leaves both to roll back. */
[[nodiscard]] InvestmentTransition
commit_and_publish(state::investment::store::Transaction& transaction,
                   const StagedFrame& frame,
                   std::uint32_t changedRows,
                   Session& session,
                   Scratch& scratch,
                   std::span<std::byte> response,
                   std::size_t& written) noexcept {
    if (!transaction.commit()) {
        return refused_transition("transaction");
    }
    publish_staged_frame(frame, session, scratch, response, written);
    return published_transition(frame, changedRows);
}

/** Grants one item through the same helper that grants world rewards. */
[[nodiscard]] InvestmentTransition publish_operation(const GrantItemOperation& request,
                                                     const InvestmentTarget& /*target*/,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    state::build_data::items::Definition item{};
    if (!state::build_data::find_item_definition_hash(request.definitionHash, item)) {
        return refused_transition("item_definition");
    }
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    StagedItemGrant staged{};
    const char* reason = nullptr;
    const ItemGrant grant{item.definitionIndex, static_cast<std::int32_t>(request.quantity)};
    switch (stage_item_grant(grant, session, scratch, response, touchesScratch, staged, reason)) {
    case ItemGrantStage::committed:
        break;
    case ItemGrantStage::waiting:
    case ItemGrantStage::deferred:
    case ItemGrantStage::unresolvable:
    case ItemGrantStage::failed:
        // The lane lends a whole frame, so nothing waits for one, and one explicit grant never
        // waits on a draw or a full inventory; the editor resubmits.
        return state_refusal_transition(reason);
    }
    if (!transaction.commit()) {
        return refused_transition("transaction");
    }
    if (!publish_item_grant(staged, session, scratch, response, written)) {
        return {.state = InvestmentReceiptState::committedUnpublished,
                .reason = staged.unpublished,
                .changedRows = staged.grantedRows};
    }
    return published_transition(staged.frame, staged.grantedRows);
}

/** Removes one unequipped instance through the dismantle transaction, crediting nothing. */
[[nodiscard]] InvestmentTransition publish_item_removal(const QuantityEdit& edit,
                                                        Session& session,
                                                        Scratch& scratch,
                                                        std::span<std::byte> response,
                                                        std::size_t& written,
                                                        bool& touchesScratch) noexcept {
    const auto pending = std::unique_ptr<state::PendingItemDismantle>(
        new (std::nothrow) state::PendingItemDismantle);
    if (!pending) {
        return refused_transition("allocation");
    }
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    const char* reason = nullptr;
    if (!state::prepare_item_removal(edit.characterSoid,
                                     edit.instanceSoid,
                                     edit.definitionHash,
                                     edit.mutationSerial,
                                     *pending,
                                     &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    queuez::ItemDismantle dismantle{};
    if (!queuez::stage_item_dismantle(session.queuez,
                                      pending->accountSoid,
                                      pending->characterSoid,
                                      pending->dismantledInstanceSoid,
                                      pending->profileChanged,
                                      dismantle)) {
        return refused_transition("inventory_stage");
    }
    StagedFrame frame{};
    frame.sendNonce = session.sendNonce;
    if (!push::append_item_dismantle_notification(scratch,
                                                  dismantle,
                                                  *pending,
                                                  session.sessionKey,
                                                  frame.sendNonce,
                                                  scratch.framed,
                                                  frame.size)
        || frame.size == 0) {
        return refused_transition("inventory_encode");
    }
    middleware::secure_channel::advance_nonce(frame.sendNonce);
    frame.after = dismantle.after;
    if (!state::commit_item_dismantle(*pending)) {
        return refused_transition("inventory_commit");
    }
    return commit_and_publish(
        transaction, frame, kEditedRowCount, session, scratch, response, written);
}

/** Replaces one row's quantity; an unequipped instance set to zero leaves as a removal. */
[[nodiscard]] InvestmentTransition publish_operation(const QuantityEdit& edit,
                                                     const InvestmentTarget& /*target*/,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    if (edit.scope == EditScope::characterItem && edit.quantity == 0) {
        return publish_item_removal(edit, session, scratch, response, written, touchesScratch);
    }
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    state::account::inventory::QuantityChange change{};
    const char* reason = nullptr;
    if (!state::account::inventory::set_quantity(edit, change, &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    StagedFrame frame{};
    // Profile rows live in the account object; character rows only in the character object.
    const ChangedObjects changed{change.accountSoid,
                                 change.characterSoid,
                                 change.characterIndex,
                                 edit.scope == EditScope::profile};
    if (!stage_changed_objects(changed, session, scratch, frame, reason)) {
        return refused_transition(reason);
    }
    return commit_and_publish(
        transaction, frame, kEditedRowCount, session, scratch, response, written);
}

/** Replaces the XP total; the artifact values it moves follow in their own refreshes. */
[[nodiscard]] InvestmentTransition publish_operation(const SeasonalExperienceOperation& request,
                                                     const InvestmentTarget& target,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    const char* reason = nullptr;
    if (!state::replace_seasonal_experience(request.expected, request.replacement, &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    StagedFrame frame{};
    if (!stage_changed_objects(
            {target.accountSoid, target.characterSoid, 0, true}, session, scratch, frame, reason)) {
        return refused_transition(reason);
    }
    const InvestmentTransition transition = commit_and_publish(
        transaction, frame, kEditedRowCount, session, scratch, response, written);
    if (transition.state == InvestmentReceiptState::published) {
        bap::arm_artifact_refresh(session);
    }
    return transition;
}

/** Claims one pass row as a client's opcode-2400 request does once it is decoded. */
[[nodiscard]] InvestmentTransition publish_operation(const SeasonPassClaimOperation& request,
                                                     const InvestmentTarget& target,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    auto pending = std::unique_ptr<state::PendingSeasonPassReward>(
        new (std::nothrow) state::PendingSeasonPassReward);
    if (!pending) {
        return refused_transition("allocation");
    }
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    const char* reason = nullptr;
    if (!web_service::prepare_season_pass_claim(request.rewardIndex, *pending, reason)) {
        return state_refusal_transition(reason);
    }
    if (pending->grant.characterSoid != target.characterSoid) {
        return refused_transition("character_changed");
    }
    ServiceOutcome outcome{};
    auto* claim = body::stage_season_pass_reward(session.queuez, *pending, outcome);
    if (claim == nullptr) {
        return refused_transition("reward_preflight");
    }
    const auto changedRows = static_cast<std::uint32_t>(pending->grant.rewardCount);
    claim->pending = std::move(pending);
    touchesScratch = true;
    StagedFrame frame{};
    frame.sendNonce = session.sendNonce;
    if (!queuez::stage_reward_outcome(scratch,
                                      session.queuez,
                                      outcome,
                                      active_acquisition_presentation_rows(session),
                                      session.sessionKey,
                                      frame.sendNonce,
                                      scratch.framed,
                                      frame.size,
                                      frame.after)
        || frame.size == 0) {
        return refused_transition("inventory_encode");
    }
    transactions::Publication publication{};
    if (!transactions::commit(outcome, publication, reason)) {
        return refused_transition(reason != nullptr ? reason : "inventory_commit");
    }
    const InvestmentTransition transition =
        commit_and_publish(transaction, frame, changedRows, session, scratch, response, written);
    if (transition.state == InvestmentReceiptState::published) {
        bap::arm_acquisition_presentation_hold(session);
    }
    return transition;
}

/** Clears one pass row's claim flag; the items it granted stay. */
[[nodiscard]] InvestmentTransition publish_operation(const SeasonPassResetOperation& request,
                                                     const InvestmentTarget& target,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    const char* reason = nullptr;
    if (!state::reset_season_pass_reward(request.rewardIndex, request.itemHash, &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    StagedFrame frame{};
    if (!stage_changed_objects(
            {target.accountSoid, target.characterSoid, 0, true}, session, scratch, frame, reason)) {
        return refused_transition(reason);
    }
    return commit_and_publish(
        transaction, frame, kEditedRowCount, session, scratch, response, written);
}

/** Publishes the objects a pursuit edit changed, then commits the operation's savepoint. */
[[nodiscard]] InvestmentTransition
publish_pursuit_edit(state::investment::store::Transaction& transaction,
                     const state::PursuitEditChange& change,
                     Session& session,
                     Scratch& scratch,
                     std::span<std::byte> response,
                     std::size_t& written) noexcept {
    StagedFrame frame{};
    const char* reason = nullptr;
    if (!stage_changed_objects(
            {change.accountSoid, change.characterSoid, change.characterIndex, change.accountRows},
            session,
            scratch,
            frame,
            reason)) {
        return refused_transition(reason);
    }
    return commit_and_publish(
        transaction, frame, change.changedRows, session, scratch, response, written);
}

/** Replaces the saved row one objective of a held pursuit reads. */
[[nodiscard]] InvestmentTransition publish_operation(const ObjectiveProgressOperation& request,
                                                     const InvestmentTarget& target,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    state::PursuitEditChange change{};
    const char* reason = nullptr;
    if (!state::set_pursuit_objective(
            {target.characterSoid, request.instanceSoid, request.itemHash, request.mutationSerial},
            request.ordinal,
            request.objectiveHash,
            request.expected,
            request.value,
            change,
            &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    return publish_pursuit_edit(transaction, change, session, scratch, response, written);
}

/** Sets each incomplete objective of a held pursuit to its target; the pursuit is not redeemed. */
[[nodiscard]] InvestmentTransition publish_operation(const ObjectiveCompletionOperation& request,
                                                     const InvestmentTarget& target,
                                                     Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    state::PursuitEditChange change{};
    const char* reason = nullptr;
    if (!state::complete_pursuit_objectives(
            {target.characterSoid, request.instanceSoid, request.itemHash, request.mutationSerial},
            change,
            &reason)) {
        return state_refusal_transition(reason);
    }
    touchesScratch = true;
    return publish_pursuit_edit(transaction, change, session, scratch, response, written);
}

/**
 * @return True while this peer still plays a presentation the operation's frame would contradict:
 * a queued XP gain against a replaced total, or the acquisition overlay a removal frame omits.
 */
[[nodiscard]] bool presentation_pending(const Session& session,
                                        const InvestmentOperation& operation) noexcept {
    if (std::holds_alternative<SeasonalExperienceOperation>(operation.request)) {
        return session.pendingSeasonalExperienceAmount != 0;
    }
    const auto* edit = std::get_if<QuantityEdit>(&operation.request);
    return edit != nullptr && edit->scope == EditScope::characterItem && edit->quantity == 0
           && GetTickCount64() < session.acquisitionPresentationUntilTick;
}

} // namespace

/** Runs one operation per poll, after this peer's resync and owed world rewards. */
bool consume_investment_operation(Session& session,
                                  Scratch& scratch,
                                  std::span<std::byte> response,
                                  std::size_t& written,
                                  bool& touchesScratch) noexcept {
    // Neither a draw nor an uncommitted edit is kept between polls, so an operation waits for a
    // buffer its whole frame is sure to fit.
    if (!session.queuez.family4Active || response.size() < scratch.framed.size()) {
        return false;
    }
    InvestmentOperation operation{};
    if (!bap::next_investment_operation_locked(session.queuez.family4RootSoid, operation)) {
        return false;
    }
    InvestmentTransition transition = bap::check_investment_operation(operation);
    if (transition.state == InvestmentReceiptState::queued) {
        transition = presentation_pending(session, operation)
                         ? deferred_transition(InvestmentWait::none)
                         : std::visit(
                               [&](const auto& request) noexcept {
                                   return publish_operation(request,
                                                            operation.target,
                                                            session,
                                                            scratch,
                                                            response,
                                                            written,
                                                            touchesScratch);
                               },
                               operation.request);
    }
    bap::record_investment_transition_locked(operation.id, transition);
    return transition.state == InvestmentReceiptState::published;
}

} // namespace sunrise::server::bap::encrypted
