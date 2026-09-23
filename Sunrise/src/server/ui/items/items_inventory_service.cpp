#include "items_inventory_service.h"

#include <limits>
#include <memory>

#include "../../../state/build_data/rewards/reward_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/progression/season_pass_reward_catalog.h"
#include "../../../state/runtime/runtime.h"
#include "../../../state/unlocks/unlocks_runtime.h"
#include "../../bap/investment_actions.h"
#include "../../bap/investment_edit.h"

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
    return bap::set_inventory_quantity(
        character, item.instance, item.hash, item.serial, item.quantity, quantity);
}

bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    const auto route = state::item_grant_route(itemIndex);
    return quantity != 0
           && quantity <= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
           && route != state::ItemGrantRoute::unavailable
           && (route != state::ItemGrantRoute::quest || quantity == 1);
}

bool grant(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    return bap::queue_item_reward(itemIndex, quantity);
}

SeasonPass season_pass(std::uint16_t rewardIndex) noexcept {
    SeasonPass result{};
    result.available = state::build_data::season_pass_ready();
    result.experience = state::seasonal_experience();
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
    bool queued = false;
    result.pending = bap::season_pass_claim_pending(rewardIndex, queued);
    result.grantable = !queued && !result.claimed && bap::investment_connected()
                       && reward.itemHash != 0 && reward.requiredRank <= result.rank;
    return result;
}

bool grant_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    return bap::queue_season_pass_reward(rewardIndex);
}

bool unclaim_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    return bap::reset_season_pass_claim(rewardIndex);
}

bool set_seasonal_experience(std::int32_t expected, std::int32_t experience) noexcept {
    return bap::set_seasonal_experience(expected, experience);
}
} // namespace sunrise::server::ui::items
