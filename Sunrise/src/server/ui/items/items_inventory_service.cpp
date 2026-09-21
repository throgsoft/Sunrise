#include "items_inventory_service.h"

#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "../../../state/account/inventory/inventory_edit.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/investment/store_internal.h"
#include "../../../state/runtime/runtime.h"
#include "../../bap/internal.h"

namespace sunrise::server::ui::items {
Inventory inventory() noexcept {
    try {
        namespace data = state::build_data;
        const auto account = std::make_unique<state::AccountState>(state::account_snapshot());
        if (!state::account::valid(*account)) return {};
        Inventory result{};
        for (std::size_t id = 0; id < data::inventory::buckets::kUnavailableBucketId; ++id) {
            data::inventory::buckets::Descriptor bucket{};
            if (data::find_inventory_bucket_descriptor(static_cast<std::uint8_t>(id), bucket))
                result.buckets.push_back({bucket.bucketId,
                                          static_cast<std::uint8_t>(bucket.arraySelector),
                                          bucket.slotCount});
        }
        const auto append = [&](std::uint64_t instance,
                                std::uint32_t hash,
                                std::int32_t quantity,
                                std::int32_t serial) {
            data::items::Definition definition{};
            if (!data::find_item_definition_hash(hash, definition)) return;
            if (instance != 0) {
                for (const auto& held : result.items)
                    if (held.instance == instance) return;
            }
            bool equipped = false;
            for (std::size_t c = 0; instance != 0 && c < account->characterCount; ++c) {
                for (const auto& slot : account->characters[c].equipment.slots)
                    equipped |= slot && slot->instanceSoid == instance;
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
            if (!character.selected) continue;
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
                if (item)
                    append(item->instanceSoid,
                           item->definitionHash,
                           item->quantity,
                           item->mutationSerial);
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
            character, item.instance, item.hash, item.serial, item.quantity, quantity))
        return false;
    bap::arm_account_resync_everywhere();
    return true;
}

bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept {
    state::build_data::items::Definition item{};
    if (quantity == 0
        || quantity > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || !state::build_data::find_item_definition_index(itemIndex, item))
        return false;
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
            || !transaction.commit())
            return false;
    }
    if (!bap::has_active_family4_peer()) bap::settle_world_reward();
    return true;
}
} // namespace sunrise::server::ui::items
