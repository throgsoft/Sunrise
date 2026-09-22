#include "items_inventory_service.h"

#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "../../../middleware/web_service/messages/opcode2400.h"
#include "../../../state/account/inventory/inventory_edit.h"
#include "../../../state/build_data/rewards/reward_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/investment/store_internal.h"
#include "../../../state/progression/season_pass_reward_catalog.h"
#include "../../../state/runtime/runtime.h"
#include "../../../state/unlocks/unlocks_runtime.h"
#include "../../bap/internal.h"

namespace sunrise::server::ui::items {

Inventory inventory() noexcept {
    try {
        namespace data = state::build_data;
        const auto account = std::make_unique<state::AccountState>(state::account_snapshot());
        if (!state::account::valid(*account)) {
            return {};
        }
        Inventory result{};
        for (std::size_t id = 0; id < data::inventory::buckets::kUnavailableBucketId; ++id) {
            data::inventory::buckets::Descriptor bucket{};
            if (data::find_inventory_bucket_descriptor(static_cast<std::uint8_t>(id), bucket)) {
                result.buckets.push_back({bucket.bucketId,
                                          static_cast<std::uint8_t>(bucket.arraySelector),
                                          bucket.slotCount});
            }
        }
        const auto append = [&](std::uint64_t instance,
                                std::uint32_t hash,
                                std::int32_t quantity,
                                std::int32_t serial) {
            data::items::Definition definition{};
            if (!data::find_item_definition_hash(hash, definition)) {
                return;
            }
            if (instance != 0) {
                for (const auto& held : result.items) {
                    if (held.instance == instance) {
                        return;
                    }
                }
            }
            bool equipped = false;
            for (std::size_t c = 0; instance != 0 && c < account->characterCount; ++c) {
                for (const auto& slot : account->characters[c].equipment.slots) {
                    equipped |= slot && slot->instanceSoid == instance;
                }
            }
            result.items.push_back({instance,
                                    hash,
                                    definition.definitionIndex,
                                    definition.bucketId,
                                    quantity,
                                    serial,
                                    equipped});
        };
        for (std::size_t i = 0; i < account->profileItemCount; ++i) {
            const auto& item = account->profileItems[i];
            append(item.instanceSoid, item.definitionHash, item.quantity, item.mutationSerial);
        }
        for (std::size_t c = 0; c < account->characterCount; ++c) {
            const auto& character = account->characters[c];
            if (!character.selected) {
                continue;
            }
            result.character = character.soid;
            for (std::size_t i = 0; i < character.inventory.count; ++i) {
                const auto& item = character.inventory.values[i];
                append(item.instanceSoid, item.definitionHash, item.quantity, item.mutationSerial);
            }
            for (std::size_t i = 0; i < character.stacks.count; ++i) {
                const auto& item = character.stacks.values[i];
                append(0, item.definitionHash, item.quantity, item.mutationSerial);
            }
            for (const auto& item : character.equipment.slots) {
                if (item) {
                    append(item->instanceSoid,
                           item->definitionHash,
                           item->quantity,
                           item->mutationSerial);
                }
            }
            break;
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool remove(std::uint64_t character, const HeldItem& item) noexcept {
    return set_quantity(character, item, 0);
}

bool set_quantity(std::uint64_t character, const HeldItem& item, std::int32_t quantity) noexcept {
    const std::lock_guard lock(bap::session_lock());
    if (!state::account::inventory::edit::set_quantity(
            character, item.instance, item.hash, item.serial, item.quantity, quantity)) {
        return false;
    }
    bap::arm_account_resync_everywhere();
    return true;
}

bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    state::build_data::items::Definition item{};
    if (quantity == 0
        || quantity > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || !state::build_data::find_item_definition_index(itemIndex, item)) {
        return false;
    }
    if (item.questInitialization.scope
        != state::build_data::items::QuestInitialization::Scope::none) {
        const std::unique_ptr<state::PendingItemAcquisition> pending(
            new (std::nothrow) state::PendingItemAcquisition);
        return pending && quantity == 1
               && state::prepare_item_acquisition_for_item(itemIndex, *pending);
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> pending(
        new (std::nothrow) state::PendingRecordRewardGrant);
    return pending && state::prepare_item_reward(itemIndex, quantity, *pending);
}

bool grant(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    const std::lock_guard lock(bap::session_lock());
    {
        state::investment::store::Transaction transaction;
        state::build_data::items::Definition item{};
        if (!transaction.ready() || !grantable(itemIndex, quantity)
            || !state::build_data::find_item_definition_index(itemIndex, item)
            || !state::investment::store::enqueue_reward(
                item.definitionHash,
                static_cast<std::int32_t>(quantity),
                static_cast<std::uint8_t>(bap::WorldRewardKind::item))
            || !transaction.commit()) {
            return false;
        }
    }
    if (!bap::has_active_family4_peer()) {
        bap::settle_world_reward();
    }
    return true;
}

SeasonPass season_pass(std::uint16_t rewardIndex) noexcept {
    const std::lock_guard sessionLock(bap::session_lock());
    const std::lock_guard lock(state::investment::store::g_mutex);
    SeasonPass result{};
    result.available = state::build_data::season_pass_ready()
                       && state::investment::store::read_unlock(
                           state::investment::store::Bank::accountProgressions,
                           state::kArtifactPowerProgressionIndex,
                           result.experience);
    if (!result.available) {
        return result;
    }
    result.rank = state::seasonal_rank();
    result.claimed = state::season_pass_reward_claimed(rewardIndex);
    state::build_data::season_pass::Reward reward{};
    const auto flag = state::build_data::find_season_pass_reward(rewardIndex, reward)
                          ? state::progression::season_pass::progress_flag(reward)
                          : state::build_data::rewards::kAbsent;
    result.progressFlag = flag != state::build_data::rewards::kAbsent;
    result.acquired = result.progressFlag && state::unlocks::account_flag_set(flag);
    if (result.progressFlag) {
        return result;
    }
    const std::unique_ptr<state::PendingSeasonPassReward> pending(
        new (std::nothrow) state::PendingSeasonPassReward);
    bool queued = false;
    for (const auto& peer : bap::sessions()) {
        if (peer.pendingSeasonPassClaim.characterSoid != 0) {
            queued = true;
            result.pending |= peer.pendingSeasonPassClaim.rewardIndex == rewardIndex;
        }
    }
    result.grantable = !queued && bap::has_active_family4_peer() && pending
                       && state::prepare_season_pass_reward(rewardIndex, *pending);
    return result;
}

bool grant_season_pass_reward(std::span<const std::byte> request) noexcept {
    const std::lock_guard lock(bap::session_lock());
    middleware::web_service::Message message{};
    middleware::web_service::messages::opcode2400::Request claim{};
    state::build_data::season_pass::Reward reward{};
    return middleware::web_service::parse_request(request, message)
           && middleware::web_service::messages::opcode2400::parse_request(message, claim)
           && state::build_data::find_season_pass_reward(claim.rewardIndex, reward)
           && state::progression::season_pass::progress_flag(reward)
                  == state::build_data::rewards::kAbsent
           && bap::queue_season_pass_claim(request);
}

bool unclaim_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    const std::lock_guard lock(bap::session_lock());
    {
        state::investment::store::Transaction transaction;
        state::build_data::season_pass::Reward reward{};
        if (!transaction.ready() || !state::season_pass_reward_claimed(rewardIndex)
            || !state::build_data::find_season_pass_reward(rewardIndex, reward)
            || state::progression::season_pass::progress_flag(reward)
                   != state::build_data::rewards::kAbsent
            || !state::unlocks::set_account_flag(reward.claimFlagIndex, state::unlocks::kFlagClear)
            || !transaction.commit()) {
            return false;
        }
    }
    bap::arm_account_resync_everywhere();
    return true;
}

bool set_seasonal_experience(std::int32_t expected, std::int32_t experience) noexcept {
    const std::lock_guard lock(bap::session_lock());
    {
        state::investment::store::Transaction transaction;
        if (experience < 0 || !transaction.ready() || state::seasonal_experience() != expected
            || !state::unlocks::set_account_progression(state::kArtifactPowerProgressionIndex,
                                                        experience)
            || !state::seed_seasonal_progression() || !transaction.commit()) {
            return false;
        }
    }
    for (auto& peer : bap::sessions()) {
        peer.pendingSeasonalExperienceAmount = 0;
        peer.pendingSeasonalExperienceMutationSerial = 0;
    }
    bap::arm_account_resync_everywhere();
    return true;
}
} // namespace sunrise::server::ui::items
