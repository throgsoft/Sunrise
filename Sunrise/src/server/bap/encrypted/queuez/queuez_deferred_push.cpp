#include <Windows.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/activity/destination/definition.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/runtime/developer_investment_runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../../state/runtime/synthesizer_crafting_runtime.h"
#include "../../developer_grant.h"
#include "../internal.h"
#include "../push/activity/activity_keepalive_push.h"
#include "dawning_pickup_release.h"
#include "queuez_state_validation.h"
#include "state/investment/store_internal.h"
#include "synthesizer_family4_refresh.h"

namespace sunrise::server::bap {
namespace {

struct DeveloperGrantRequest {
    DeveloperGrantReceipt receipt{};
    state::developer::ItemGrantTarget target{};
    std::uint64_t queuedAt{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::uint16_t definitionIndex{};
};
constexpr std::uint64_t kDeveloperGrantLifetimeMs = 120'000;
std::mutex g_developerGrantMutex;
std::array<DeveloperGrantRequest, 8> g_developerGrants{};
std::uint64_t g_nextDeveloperGrantId{1};

void expire_developer_grants(std::uint64_t now) noexcept {
    for (auto& request : g_developerGrants) {
        if (request.receipt.status == DeveloperGrantStatus::queued
            && now - request.queuedAt >= kDeveloperGrantLifetimeMs) {
            request.receipt.status = DeveloperGrantStatus::refused;
            request.receipt.reason = "grant expired before F4 publication; nothing committed";
        }
    }
}

// Only the BAP owner takes work; the render thread holds this separate lock for copies only.
bool take_developer_grant(std::uint64_t accountSoid, DeveloperGrantRequest& output) noexcept {
    const std::lock_guard lock(g_developerGrantMutex);
    expire_developer_grants(GetTickCount64());
    DeveloperGrantRequest* first = nullptr;
    for (auto& request : g_developerGrants)
        if (request.receipt.status == DeveloperGrantStatus::queued
            && (!first || request.receipt.id < first->receipt.id))
            first = &request;
    if (!first || first->target.accountSoid != accountSoid) return false;
    first->receipt.status = DeveloperGrantStatus::publishing;
    first->receipt.reason = "preparing authoritative F4 publication";
    output = *first;
    return true;
}

void finish_developer_grant(const DeveloperGrantRequest& request,
                            DeveloperGrantReceipt receipt) noexcept {
    {
        const std::lock_guard lock(g_developerGrantMutex);
        for (auto& slot : g_developerGrants) {
            if (slot.receipt.id == request.receipt.id) {
                slot.receipt = receipt;
                break;
            }
        }
    }
    if (receipt.status == DeveloperGrantStatus::queued) return;
    core::log::writef(core::log::Channel::server,
                      receipt.status == DeveloperGrantStatus::refused ? core::log::Level::warn
                                                                      : core::log::Level::info,
                      "ev=developer_grant request=%llu item=%u quantity=%d status=%u "
                      "changed=%zu inventory=%zu postmaster=%zu material=%zu f4=%d reason=%s",
                      static_cast<unsigned long long>(receipt.id),
                      unsigned(request.definitionIndex),
                      request.quantity,
                      unsigned(receipt.status),
                      receipt.changed,
                      receipt.inventoryCount,
                      receipt.postmasterCount,
                      receipt.materialCount,
                      receipt.family4Version,
                      receipt.reason);
}

} // namespace

DeveloperGrantReceipt enqueue_developer_item_grant(std::uint16_t index,
                                                   std::int32_t quantity,
                                                   std::uint32_t expectedHash) noexcept {
    DeveloperGrantReceipt refused{};
    refused.status = DeveloperGrantStatus::refused;
    if (quantity < 1 || expectedHash == 0) {
        refused.reason = "positive quantity and installed identity required";
        return refused;
    }
    state::developer::ItemGrantTarget target{};
    if (!state::developer::read_item_grant_target(target)) {
        refused.reason = "no selected account/character available";
        return refused;
    }
    const std::lock_guard lock(g_developerGrantMutex);
    const auto now = GetTickCount64();
    expire_developer_grants(now);
    DeveloperGrantRequest* slot = nullptr;
    for (auto& candidate : g_developerGrants) {
        if (candidate.receipt.status == DeveloperGrantStatus::queued
            || candidate.receipt.status == DeveloperGrantStatus::publishing)
            continue;
        if (!slot || candidate.receipt.id < slot->receipt.id) slot = &candidate;
    }
    if (!slot || g_nextDeveloperGrantId == (std::numeric_limits<std::uint64_t>::max)()) {
        refused.reason = "developer grant queue full; nothing committed";
        return refused;
    }
    *slot = {};
    slot->target = target;
    slot->queuedAt = now;
    slot->definitionIndex = index;
    slot->definitionHash = expectedHash;
    slot->quantity = quantity;
    slot->receipt.id = g_nextDeveloperGrantId++;
    slot->receipt.status = DeveloperGrantStatus::queued;
    slot->receipt.reason = "queued for authoritative acquisition; nothing committed yet";
    return slot->receipt;
}

DeveloperGrantReceipt developer_item_grant_receipt(std::uint64_t id) noexcept {
    if (id == 0) return {};
    const std::lock_guard lock(g_developerGrantMutex);
    expire_developer_grants(GetTickCount64());
    for (const auto& request : g_developerGrants)
        if (request.receipt.id == id) return request.receipt;
    return {};
}

} // namespace sunrise::server::bap

namespace sunrise::server::bap::encrypted {
namespace {

/** @return The peer's retained row overlay, or empty once its presentation hold has passed. */
[[nodiscard]] std::span<const queuez::AcquisitionPresentationRow>
active_acquisition_presentation_rows(const Session& session) noexcept {
    if (GetTickCount64() >= session.acquisitionPresentationUntilTick
        || session.acquisitionPresentationRowCount > session.acquisitionPresentationRows.size()) {
        return {};
    }
    return std::span(session.acquisitionPresentationRows)
        .first(session.acquisitionPresentationRowCount);
}

/** Drops the visual XP notification and republishes the account, which already holds the XP. */
void drop_seasonal_experience_presentation(Session& session) noexcept {
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    bap::arm_account_resync_everywhere();
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=season_xp stage=deferred_presentation result=drop");
}

/** @return The picked character, or null when the account is invalid or nothing is picked. */
[[nodiscard]] const state::CharacterState*
selected_character(const state::AccountState& account) noexcept {
    if (!state::account::valid(account)) {
        return nullptr;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return &account.characters[index];
        }
    }
    return nullptr;
}

/** One bounded developer batch uses the same State/default-plug and acquisition-ring path as
 * record rewards. A failed encode or commit never settles it silently into the account.
 */
[[nodiscard]] bool consume_developer_item_grant(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept {
    if (!session.queuez.family4Active || GetTickCount64() < bap::acquisition_queue_deadline())
        return false;
    DeveloperGrantRequest request{};
    if (!take_developer_grant(session.queuez.family4RootSoid, request)) return false;
    auto receipt = request.receipt;
    const auto refuse = [&](const char* reason) noexcept {
        receipt.status = DeveloperGrantStatus::refused;
        receipt.reason = reason;
        finish_developer_grant(request, receipt);
        return false;
    };
    state::investment::store::Transaction transaction;
    if (!transaction.ready())
        return refuse("investment transaction unavailable; nothing committed");
    const std::unique_ptr<state::PendingRecordRewardGrant> pending(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!pending) return refuse("grant allocation failed; nothing committed");
    const auto result = state::developer::prepare_item_grant(request.definitionIndex,
                                                             request.quantity,
                                                             request.definitionHash,
                                                             request.target,
                                                             *pending);
    if (!result.accepted) return refuse(result.reason);
    if (!pending->prepared) {
        receipt.status = DeveloperGrantStatus::unchanged;
        receipt.reason = result.reason;
        finish_developer_grant(request, receipt);
        return false;
    }
    std::array<std::uint64_t, state::kRecordRewardGrantCapacity> residents{};
    std::size_t residentCount = 0;
    for (std::size_t i = 0; i < pending->rewardCount; ++i) {
        const auto& reward = pending->rewards[i];
        if (reward.kind == state::RecordRewardKind::characterInstance
            || reward.appendedProfileResident)
            residents[residentCount++] = reward.instanceSoid;
    }
    queuez::RecordRewardGrant update{};
    if (!queuez::stage_record_reward_grant(session.queuez,
                                           pending->accountSoid,
                                           pending->characterSoid,
                                           std::span(residents).first(residentCount),
                                           update))
        return refuse("F4 resident staging refused; nothing committed");
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_record_reward_notification(scratch,
                                                 session.queuez,
                                                 update,
                                                 *pending,
                                                 active_acquisition_presentation_rows(session),
                                                 session.sessionKey,
                                                 nextSendNonce,
                                                 scratch.framed,
                                                 framedSize)
        || framedSize == 0)
        return refuse("F4 acquisition encoding refused; nothing committed");
    if (framedSize > response.size()) {
        receipt.status = DeveloperGrantStatus::queued;
        receipt.reason = "waiting for a complete BAP response buffer; nothing committed";
        finish_developer_grant(request, receipt);
        return false;
    }
    const bool changesMoteOwnership = state::runtime::detail::synthesizer::mote_ownership_changed(
        pending->beforeProfileItems, pending->afterProfileItems);
    receipt.changed = pending->rewardCount;
    for (std::size_t i = 0; i < pending->rewardCount; ++i) {
        const auto& reward = pending->rewards[i];
        if (reward.kind == state::RecordRewardKind::accountMaterial)
            ++receipt.materialCount;
        else if (reward.kind == state::RecordRewardKind::characterInstance
                 && pending->afterCharacter.inventory.values[reward.stateIndex].placement
                        == state::account::inventory::ItemPlacement::postmaster)
            ++receipt.postmasterCount;
        else
            ++receipt.inventoryCount;
    }
    if (!state::commit_record_reward(*pending) || !transaction.commit()) {
        receipt.changed = receipt.inventoryCount = receipt.postmasterCount = receipt.materialCount =
            0;
        return refuse("State commit failed; whole batch rolled back");
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    bap::arm_account_resync_elsewhere(session);
    bap::arm_acquisition_presentation_hold(session);
    if (changesMoteOwnership) session.family5RefreshArmed = true;
    receipt.status = DeveloperGrantStatus::published;
    receipt.family4Version = update.after.family4Version;
    receipt.reason = "grant committed and F4 acquisition published";
    finish_developer_grant(request, receipt);
    return true;
}

/** Publishes and commits one character-inventory world reward. */
[[nodiscard]] bool consume_world_item_acquisition(const WorldRewardRequest& request,
                                                  Session& session,
                                                  Scratch& scratch,
                                                  std::span<std::byte> response,
                                                  std::size_t& written,
                                                  bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }

    state::PendingItemAcquisition pending{};
    if (!state::prepare_item_acquisition_for_item(request.itemDefinitionIndex, pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=prepare");
        bap::settle_world_reward();
        return false;
    }
    touchesScratch = true;
    queuez::ItemAcquisition acquisition{};
    if (!queuez::stage_item_acquisition(session.queuez,
                                        pending.accountSoid,
                                        pending.characterSoid,
                                        pending.acquiredInstanceSoid,
                                        pending.updates_account(),
                                        acquisition)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=stage");
        bap::settle_world_reward();
        return false;
    }
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_item_acquisition_notification(scratch,
                                                    acquisition,
                                                    pending,
                                                    active_acquisition_presentation_rows(session),
                                                    session.sessionKey,
                                                    nextSendNonce,
                                                    scratch.framed,
                                                    framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=encode");
        bap::settle_world_reward();
        return false;
    }
    if (!state::commit_item_acquisition(pending) || !bap::complete_world_reward(request.id)
        || !transaction.commit()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=commit");
        bap::settle_world_reward();
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = acquisition.after;
    bap::arm_account_resync_elsewhere(session);
    bap::arm_acquisition_presentation_hold(session);
    return true;
}

/** Publishes and commits one profile-inventory world reward. */
[[nodiscard]] bool consume_world_profile_item_acquisition(const WorldRewardRequest& request,
                                                          Session& session,
                                                          Scratch& scratch,
                                                          std::span<std::byte> response,
                                                          std::size_t& written,
                                                          bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }

    state::PendingProfileItemAcquisition pending{};
    if (!state::prepare_profile_item_acquisition_for_item(
            request.itemDefinitionIndex, request.quantity, pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=prepare");
        bap::settle_world_reward();
        return false;
    }
    const bool changesMoteOwnership = state::runtime::detail::synthesizer::mote_ownership_changed(
        pending.beforeItems, pending.afterItems);
    touchesScratch = true;
    queuez::ProfileItemAcquisition acquisition{};
    if (!queuez::stage_profile_item_acquisition(session.queuez,
                                                pending.accountSoid,
                                                pending.acquiredInstanceSoid,
                                                pending.actionSource,
                                                pending.appended,
                                                acquisition)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=stage");
        bap::settle_world_reward();
        return false;
    }
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_profile_item_acquisition_notification(scratch,
                                                            acquisition,
                                                            pending,
                                                            session.sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=encode");
        bap::settle_world_reward();
        return false;
    }
    if (!state::commit_profile_item_acquisition(pending) || !bap::complete_world_reward(request.id)
        || !transaction.commit()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=commit");
        bap::settle_world_reward();
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = acquisition.after;
    if (changesMoteOwnership) session.family5RefreshArmed = true;
    bap::arm_account_resync_elsewhere(session);
    bap::arm_acquisition_presentation_hold(session);
    return true;
}

/** Publishes one non-persistent XP reward row so the native seasonal XP HUD animates. */
[[nodiscard]] bool consume_seasonal_experience_presentation(Session& session,
                                                            Scratch& scratch,
                                                            std::span<std::byte> response,
                                                            std::size_t& written,
                                                            bool& touchesScratch) noexcept {
    if (session.pendingSeasonalExperienceAmount <= 0) {
        return false;
    }
    if (session.pendingSeasonalExperienceMutationSerial == 0) {
        std::int32_t mutationSerial = 0;
        // The row belongs to the picked character, so the gain waits for a pick.
        if (!state::reserve_selected_character_inventory_serial(mutationSerial)) {
            return false;
        }
        session.pendingSeasonalExperienceMutationSerial =
            static_cast<std::uint32_t>(mutationSerial) + 1U;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    if (!push::append_seasonal_experience_notification(
            scratch,
            session.queuez,
            session.pendingSeasonalExperienceAmount,
            static_cast<std::int32_t>(session.pendingSeasonalExperienceMutationSerial - 1U),
            active_acquisition_presentation_rows(session),
            session.sessionKey,
            nextSendNonce,
            scratch.framed,
            framedSize,
            after)
        || framedSize == 0 || framedSize > response.size()) {
        drop_seasonal_experience_presentation(session);
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    bap::arm_account_resync_elsewhere(session);
    return true;
}

/** Publishes the current account graph to a peer invalidated by another connection. */
[[nodiscard]] bool consume_account_resync(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    if (!session.accountResyncArmed) {
        return false;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState currentQueuez{};
    if (!push::append_account_resync_notification(scratch,
                                                  session.queuez,
                                                  active_acquisition_presentation_rows(session),
                                                  session.sessionKey,
                                                  nextSendNonce,
                                                  scratch.framed,
                                                  framedSize,
                                                  currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=family4");
        return false;
    }
    bool auxiliaryRefreshFailed = false;
    if (currentQueuez.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 currentQueuez,
                                                                 session.sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family0");
            auxiliaryRefreshFailed = true;
        } else {
            currentQueuez = appearanceAfter;
        }
    }
    if (currentQueuez.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(scratch,
                                                             currentQueuez,
                                                             session.sessionKey,
                                                             nextSendNonce,
                                                             scratch.framed,
                                                             framedSize,
                                                             rosterAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family3");
            auxiliaryRefreshFailed = true;
        } else {
            currentQueuez = rosterAfter;
        }
    }
    if (framedSize == 0 || framedSize > response.size() || !queuez::valid(currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=output");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = currentQueuez;
    session.accountResyncArmed = false;
    // Console grants/discards and mutations on other peers reach this path. Their
    // inventory-derived Mote flags must follow the newly published account view.
    session.family5RefreshArmed = true;
    if (auxiliaryRefreshFailed) {
        // Family 4 already produced a complete frame. Appearance and roster are derived views,
        // so they retry in their own deferred lane rather than holding the account update.
        session.abilityRefreshDueTick = GetTickCount64();
        session.characterRefreshScope = CharacterRefreshScope::recordsAndRoster;
    }
    return true;
}

/** Sends the owed banner retry after its delay. */
[[nodiscard]] bool consume_banner_repush(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    if (!session.bannerRepushArmed || session.bannerRepushRoot == 0
        || GetTickCount64() < session.bannerRepushDueTick) {
        return false;
    }
    // Retain the arm until the account has a character to name.
    if (state::account::banner_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;

    // Reuse the subscription path so its version and the host mirror stay aligned.
    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kBannerFamilyType;
    subscription.familyRootSoid = session.bannerRepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState bannerAfter{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     session.sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     bannerAfter,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // The frame is committed here, so the recorded delivery and the arm are committed with it.
    if (valid(bannerAfter)) {
        session.queuez = bannerAfter;
    }
    session.bannerRepushArmed = false;
    return true;
}

/**
 * Sends the family-two re-push the equip that moved the member record owes.
 * An emblem equip leaves the subscribe-time snapshot stale, so the body is rebuilt against the
 * root the subscribe was answered with. One attempt: the arm is spent before the frame is
 * built.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole family-two notification is published.
 */
[[nodiscard]] bool consume_social_roster_repush(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept {
    if (!session.socialRosterRepushArmed || session.socialRosterRepushRoot == 0) {
        return false;
    }
    // Spent up front, so no path below can leave it owed.
    session.socialRosterRepushArmed = false;
    touchesScratch = true;

    // The same body the subscribe answer builds, rebuilt against current State so the emblem it
    // carries is the one now worn.
    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kSocialRosterFamilyType;
    subscription.familyRootSoid = session.socialRosterRepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState rosterAfter{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     session.sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     rosterAfter,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=social_roster_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (valid(rosterAfter)) {
        session.queuez = rosterAfter;
    }
    return true;
}

/** Refreshes character records after an asynchronous ability-bucket rebuild. */
[[nodiscard]] bool consume_ability_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    if (session.characterRefreshScope == CharacterRefreshScope::none
        || GetTickCount64() < session.abilityRefreshDueTick) {
        return false;
    }
    // Retain the arm until a family that reads abilities is active.
    if (!session.queuez.family0Active && !session.queuez.family3Active) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState current = session.queuez;
    bool wrote = false;
    if (current.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 current,
                                                                 session.sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            return false;
        }
        current = appearanceAfter;
        wrote = true;
    }
    if (current.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(
                scratch,
                current,
                session.sessionKey,
                nextSendNonce,
                scratch.framed,
                framedSize,
                rosterAfter,
                session.characterRefreshScope == CharacterRefreshScope::recordsAndRoster)) {
            return false;
        }
        current = rosterAfter;
        wrote = true;
    }
    if (!wrote || framedSize == 0 || framedSize > response.size() || !queuez::valid(current)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=ability_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = current;
    // Clear the arm only after publication.
    session.characterRefreshScope = CharacterRefreshScope::none;
    return true;
}

/**
 * Publishes current account unlock overrides without scheduling a Family-4 rebuild.
 * Retain the arm until the frame fits and is published, including on a transient failure.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when the family-five snapshot is published.
 */
[[nodiscard]] bool consume_family5_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    if (!session.family5RefreshArmed) {
        return false;
    }
    if (session.queuez.family5Version == (std::numeric_limits<std::int32_t>::max)()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=family5_refresh result=fail reason=version");
        return false;
    }
    touchesScratch = true;
    const std::int32_t version = session.queuez.family5Version + 1;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    auto publishedMoteMask = session.queuez.publishedMoteMask;
    if (!push::append_family5_override_notification(scratch,
                                                    session.queuez.publishedMoteMask,
                                                    publishedMoteMask,
                                                    version,
                                                    session.sessionKey,
                                                    nextSendNonce,
                                                    scratch.framed,
                                                    framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=family5_refresh result=fail reason=frame");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez.family5Version = version;
    session.family5RefreshArmed = false;
    if (session.queuez.publishedMoteMask != publishedMoteMask)
        core::log::writef(core::log::Channel::server,
                          core::log::Level::info,
                          "ev=synthesizer_visibility stage=published version=%d "
                          "previous_mask=0x%03X held_mask=0x%03X",
                          version,
                          static_cast<unsigned>(session.queuez.publishedMoteMask),
                          static_cast<unsigned>(publishedMoteMask));
    session.queuez.publishedMoteMask = publishedMoteMask;
    arm_synthesizer_family4_refresh(session.synthesizerFamily4Refresh, GetTickCount64());
    return true;
}

/** Re-publishes only the selected character after an artifact purchase. */
[[nodiscard]] bool consume_artifact_family4_refresh(Session& session,
                                                    Scratch& scratch,
                                                    std::span<std::byte> response,
                                                    std::size_t& written,
                                                    bool& touchesScratch) noexcept {
    if (!session.artifactFamily4RefreshArmed
        || GetTickCount64() < session.artifactFamily4RefreshDueTick) {
        return false;
    }
    const state::AccountState account = state::account_snapshot();
    const state::CharacterState* selected = selected_character(account);
    if (selected == nullptr) {
        return false;
    }
    state::PendingArtifactPurchase refresh{};
    refresh.accountSoid = account.primarySoid;
    refresh.characterSoid = selected->soid;
    refresh.characterIndex = static_cast<std::size_t>(selected - account.characters.data());
    refresh.beforeMask = state::artifact_mod_mask();
    refresh.afterMask = refresh.beforeMask;
    refresh.prepared = true;

    queuez::EquipmentSwap update{};
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    touchesScratch = true;
    const auto presentationRows = active_acquisition_presentation_rows(session);
    if (!queuez::stage_equipment_swap(session.queuez, refresh.characterSoid, update)
        || !push::append_artifact_purchase_notification(scratch,
                                                        update,
                                                        refresh,
                                                        presentationRows,
                                                        session.sessionKey,
                                                        nextSendNonce,
                                                        scratch.framed,
                                                        framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=artifact_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    session.artifactFamily4RefreshArmed = false;
    session.artifactFamily4RefreshDueTick = 0;
    return true;
}

/** Publishes one reset-affected item resident per poll using the proven socket-update shape. */
[[nodiscard]] bool consume_artifact_item_refresh(Session& session,
                                                 Scratch& scratch,
                                                 std::span<std::byte> response,
                                                 std::size_t& written,
                                                 bool& touchesScratch) noexcept {
    if (session.artifactResetRefreshCursor >= session.artifactResetRefresh.instanceCount) {
        session.artifactResetRefresh = {};
        session.artifactResetRefreshCursor = 0;
        return false;
    }
    const state::AccountState account = state::account_snapshot();
    const state::CharacterState* selected = selected_character(account);
    if (selected == nullptr) {
        return false;
    }
    const std::uint64_t instanceSoid =
        session.artifactResetRefresh.instanceSoids[session.artifactResetRefreshCursor];
    queuez::EquipmentSwap update{};
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    touchesScratch = true;
    if (!queuez::stage_equipment_swap(session.queuez, selected->soid, update)
        || !push::append_artifact_item_refresh_notification(scratch,
                                                            update,
                                                            instanceSoid,
                                                            session.sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=artifact_item_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    ++session.artifactResetRefreshCursor;
    if (session.artifactResetRefreshCursor >= session.artifactResetRefresh.instanceCount) {
        // Equipped sockets feed Family 0/3's derived perk banks. Refresh them once every
        // changed item resident has landed so reset cannot leave the previous champion effect
        // cached.
        session.abilityRefreshDueTick = GetTickCount64();
        if (session.characterRefreshScope == CharacterRefreshScope::none) {
            session.characterRefreshScope = CharacterRefreshScope::records;
        }
    }
    return true;
}

} // namespace

/** Publishes the next due reward, refresh, retry, or keepalive. */
bool consume_deferred(Session& session,
                      Scratch& scratch,
                      std::span<std::byte> response,
                      std::size_t& written,
                      bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated) {
        return false;
    }
    // Publish predicates before refreshing the item views that consume them.
    if (consume_family5_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_synthesizer_family4_refresh(session,
                                            session.synthesizerFamily4Refresh,
                                            scratch,
                                            response,
                                            written,
                                            touchesScratch)) {
        return true;
    }
    if (consume_artifact_family4_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_artifact_item_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_account_resync(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A failed resync blocks every incremental that could depend on its missing objects.
    if (session.accountResyncArmed) {
        return false;
    }
    if (consume_dawning_pickup_release(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_developer_item_grant(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    WorldRewardRequest reward{};
    if (session.queuez.family4Active && bap::current_world_reward(reward)) {
        bool published = false;
        switch (reward.kind) {
        case WorldRewardKind::item:
            published = consume_world_item_acquisition(
                reward, session, scratch, response, written, touchesScratch);
            break;
        case WorldRewardKind::profileItem:
            published = consume_world_profile_item_acquisition(
                reward, session, scratch, response, written, touchesScratch);
            break;
        }
        if (published) {
            return true;
        }
    }
    if (consume_seasonal_experience_presentation(
            session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_ability_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick
        || GetTickCount64() < session.acquisitionPresentationUntilTick) {
        return consume_social_roster_repush(session, scratch, response, written, touchesScratch)
               || consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    // One attempt is owed, and it is spent whether or not it lands.
    touchesScratch = true;

    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kAccountFamilyType;
    subscription.familyRootSoid = session.family4RepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     session.sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        // Neither failure clears on a retry. Holding the arm starves the keepalive, and the
        // client drops the activity session once the keepalive stops.
        session.family4RepushArmed = false;
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         framedSize == 0 ? "ev=queuez stage=repush result=fail reason=encode"
                                         : "ev=queuez stage=repush result=fail reason=capacity");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    session.family4RepushArmed = false;
    return true;
}

} // namespace sunrise::server::bap::encrypted
