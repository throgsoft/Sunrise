#include "investment_actions.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "internal.h"
#include "middleware/web_service/messages/opcode2400.h"
#include "state/build_data/runtime.h"
#include "state/progression/season_pass_reward_catalog.h"

namespace sunrise::server::bap {
bool queue_item_reward(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    const std::lock_guard lock(session_lock());
    const auto fail = [&](const char* reason) noexcept {
        report_reward_refusal("item_queue", itemIndex, reason);
        return false;
    };
    if (quantity == 0
        || quantity > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return fail("quantity");
    }
    WorldRewardKind kind = WorldRewardKind::item;
    switch (state::item_grant_route(itemIndex)) {
    case state::ItemGrantRoute::quest: {
        const auto pending = std::unique_ptr<state::PendingItemAcquisition>(
            new (std::nothrow) state::PendingItemAcquisition);
        if (quantity != 1 || !pending
            || !state::prepare_item_acquisition_for_item(itemIndex, *pending)) {
            return fail("quest_preparation");
        }
        break;
    }
    case state::ItemGrantRoute::profile: {
        const auto pending = std::unique_ptr<state::PendingProfileItemAcquisition>(
            new (std::nothrow) state::PendingProfileItemAcquisition);
        if (!pending
            || !state::prepare_profile_item_acquisition_for_item(
                itemIndex, static_cast<std::int32_t>(quantity), *pending)) {
            return fail("profile_preparation");
        }
        kind = WorldRewardKind::profileItem;
        break;
    }
    case state::ItemGrantRoute::reward: {
        const auto pending = std::unique_ptr<state::PendingRecordRewardGrant>(
            new (std::nothrow) state::PendingRecordRewardGrant);
        const char* reason = "storage";
        if (!pending || !state::prepare_item_reward(itemIndex, quantity, *pending, &reason)) {
            return fail(reason);
        }
        break;
    }
    default:
        return fail("item_route");
    }
    return enqueue_world_reward(itemIndex, static_cast<std::int32_t>(quantity), kind)
           || fail("queue_write");
}

bool investment_connected() noexcept {
    const std::lock_guard lock(session_lock());
    return has_active_family4_peer();
}

bool season_pass_claim_pending(std::uint16_t rewardIndex, bool& busy) noexcept {
    const std::lock_guard lock(session_lock());
    bool pending = false;
    busy = false;
    for (const auto& peer : sessions()) {
        if (peer.pendingSeasonPassClaim.characterSoid != 0) {
            busy = true;
            pending |= peer.pendingSeasonPassClaim.rewardIndex == rewardIndex;
        }
    }
    return pending;
}

bool queue_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    const std::lock_guard lock(session_lock());
    const auto fail = [&](const char* reason) noexcept {
        report_reward_refusal("pass_queue", rewardIndex, reason);
        return false;
    };
    state::build_data::season_pass::Reward reward{};
    if (!state::build_data::find_season_pass_reward(rewardIndex, reward)) {
        return fail("reward_index");
    }
    if (state::progression::season_pass::progress_flag(reward)
        != state::build_data::rewards::kAbsent) {
        return fail("automatic_reward");
    }
    if (std::any_of(sessions().begin(), sessions().end(), [](const Session& session) {
            return session.pendingSeasonPassClaim.characterSoid != 0;
        })) {
        return fail("pending_claim");
    }
    const std::unique_ptr<state::PendingSeasonPassReward> pending(
        new (std::nothrow) state::PendingSeasonPassReward);
    const char* reason = "storage";
    if (!pending || !state::prepare_season_pass_reward(rewardIndex, *pending, &reason)) {
        return fail(reason);
    }
    namespace codec = middleware::web_service::messages::opcode2400;
    std::array<std::byte, codec::kRequestSize> request{};
    std::size_t written = 0;
    if (!codec::encode_request(
            {state::progression::season_pass::kProgressionDefinitionIndex, rewardIndex},
            0,
            request,
            written)) {
        return fail("claim_encode");
    }
    for (auto& peer : sessions()) {
        if (peer.id != 0 && peer.authenticated && peer.queuez.family4Active
            && peer.queuez.family4RootSoid == pending->grant.accountSoid) {
            peer.pendingSeasonPassClaim = {pending->grant.characterSoid, rewardIndex};
            std::copy(request.begin(), request.end(), peer.pendingSeasonPassClaim.body.begin());
            return true;
        }
    }
    return fail("subscription");
}

} // namespace sunrise::server::bap
