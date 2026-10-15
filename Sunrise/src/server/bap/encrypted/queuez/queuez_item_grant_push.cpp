#include "queuez_item_grant_push.h"

#include <algorithm>
#include <memory>
#include <new>
#include <span>

#include "../../../../middleware/crypto/random_bytes.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Records why a committed grant has no frame, or moves the nonce past the frame it has. */
void close_staged_grant(StagedItemGrant& staged,
                        bool stagedUpdate,
                        bool encoded,
                        const queuez::SessionState& after) noexcept {
    if (!encoded) {
        staged.unpublished = !stagedUpdate ? "inventory_stage" : "inventory_encode";
        return;
    }
    middleware::secure_channel::advance_nonce(staged.frame.sendNonce);
    staged.frame.after = after;
}

/** Draws, stages and commits one item resolved through its installed reward definition. */
[[nodiscard]] ItemGrantStage stage_reward_definition_grant(const ItemGrant& grant,
                                                           Session& session,
                                                           Scratch& scratch,
                                                           std::span<std::byte> response,
                                                           bool& touchesScratch,
                                                           StagedItemGrant& staged,
                                                           const char*& reason) noexcept {
    // A draw is not retained, so it waits for a buffer its whole frame is sure to fit.
    if (response.size() < scratch.framed.size()) {
        return ItemGrantStage::waiting;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> pending(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!pending) {
        reason = "reward_allocation";
        return ItemGrantStage::failed;
    }
    std::uint64_t seed = 0;
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)))) {
        reason = "random_source";
        return ItemGrantStage::failed;
    }
    const auto preparation = state::prepare_item_reward(grant.itemDefinitionIndex,
                                                        static_cast<std::uint32_t>(grant.quantity),
                                                        seed,
                                                        *pending,
                                                        &reason);
    if (preparation == state::RewardPreparation::unresolvable) {
        return ItemGrantStage::unresolvable;
    }
    if (preparation != state::RewardPreparation::prepared) {
        return ItemGrantStage::deferred;
    }
    touchesScratch = true;
    queuez::RecordRewardGrant update{};
    staged.frame.sendNonce = session.sendNonce;
    const bool stagedUpdate = queuez::stage_record_reward_grant(session.queuez, *pending, update);
    const bool encoded =
        stagedUpdate
        && push::append_record_reward_notification(scratch,
                                                   session.queuez,
                                                   update,
                                                   *pending,
                                                   active_acquisition_presentation_rows(session),
                                                   session.sessionKey,
                                                   staged.frame.sendNonce,
                                                   scratch.framed,
                                                   staged.frame.size)
        && staged.frame.size != 0;
    staged.grantedRows = static_cast<std::uint32_t>(pending->rowCount);
    if (!state::commit_record_reward(*pending)) {
        reason = "inventory_commit";
        return ItemGrantStage::failed;
    }
    close_staged_grant(staged, stagedUpdate, encoded, update.after);
    return ItemGrantStage::committed;
}

} // namespace

/** Every grant goes through State's one placement path, which overflows the way the client does. */
ItemGrantStage stage_item_grant(const ItemGrant& grant,
                                Session& session,
                                Scratch& scratch,
                                std::span<std::byte> response,
                                bool& touchesScratch,
                                StagedItemGrant& staged,
                                const char*& reason) noexcept {
    staged = {};
    reason = nullptr;
    return stage_reward_definition_grant(
        grant, session, scratch, response, touchesScratch, staged, reason);
}

/** Copies the frame out, then publishes its nonce and after-image and arms every other peer. */
void publish_staged_frame(const StagedFrame& frame,
                          Session& session,
                          Scratch& scratch,
                          std::span<std::byte> response,
                          std::size_t& written) noexcept {
    std::copy_n(scratch.framed.begin(), frame.size, response.begin());
    written = frame.size;
    session.sendNonce = frame.sendNonce;
    session.queuez = frame.after;
    bap::arm_account_resync_elsewhere(session);
}

/** No buffer fixes a failed stage or encode, so a committed grant without a frame resyncs. */
bool publish_item_grant(const StagedItemGrant& staged,
                        Session& session,
                        Scratch& scratch,
                        std::span<std::byte> response,
                        std::size_t& written) noexcept {
    if (staged.unpublished != nullptr) {
        bap::arm_account_resync_everywhere();
        return false;
    }
    publish_staged_frame(staged.frame, session, scratch, response, written);
    bap::arm_acquisition_presentation_hold(session);
    return true;
}

} // namespace sunrise::server::bap::encrypted
