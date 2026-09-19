#include "dawning_reward_runtime.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "fifo_bucket_eviction.h"

namespace sunrise::state::runtime::detail::dawning {
namespace identity = account::inventory::dawning;
namespace buckets = build_data::inventory::buckets;
namespace {

bool delivery_lane(std::uint32_t hash, buckets::Descriptor& bucket) noexcept {
    build_data::items::Definition item{};
    build_data::items::details::Definition detail{};
    return build_data::find_item_definition_hash(hash, item)
           && build_data::find_configured_item_detail(item.definitionIndex, detail)
           && detail.definitionHash == hash && detail.bucketId == item.bucketId
           && detail.instancedDefinitionState
                  == build_data::items::details::InstancedDefinitionState::stackable
           && detail.maxStackSize == 1 && !detail.equipmentSlot
           && build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
           && bucket.arraySelector == buckets::ArraySelector::character && bucket.slotCount > 0
           && bucket.slotCount <= account::inventory::kCharacterStackCapacity
           && bucket.policyFlags == (buckets::kFifo | buckets::kNoTransferOnEviction);
}

bool pickup_bucket(std::uint32_t hash, buckets::Descriptor& bucket) noexcept {
    const auto index = identity::ingredient(hash);
    return index < identity::kIngredientCount && hash == identity::kIngredients[index].pickupHash
           && delivery_lane(hash, bucket);
}

bool pickup_space(const CharacterState& character,
                  const buckets::Descriptor& bucket,
                  std::size_t& available) noexcept {
    std::size_t occupied = 0;
    const auto count = [&](std::uint32_t hash) {
        build_data::items::Definition item{};
        if (!build_data::find_item_definition_hash(hash, item)) return false;
        occupied += item.bucketId == bucket.bucketId;
        return true;
    };
    for (std::size_t i = 0; i < character.stacks.count; ++i)
        if (!count(character.stacks.values[i].definitionHash)) return false;
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        const auto& item = character.inventory.values[i];
        if (item.placement == account::inventory::ItemPlacement::inventory
            && !count(item.definitionHash))
            return false;
    }
    for (const auto& item : character.equipment.slots)
        if (item && !count(item->definitionHash)) return false;
    if (occupied > bucket.slotCount) return false;
    available = (std::min)(static_cast<std::size_t>(bucket.slotCount) - occupied,
                           character.stacks.values.size() - character.stacks.count);
    return true;
}

/** Inventory entries carry acquisition identity; the oven counter carries material quantity.
 * Queued grants wait for space rather than overwriting an unconsumed pickup.
 */
bool insert_pickups(CharacterState& character,
                    std::uint32_t hash,
                    std::int32_t quantity,
                    std::int32_t& lastSerial) noexcept {
    buckets::Descriptor bucket{};
    if (quantity <= 0 || !pickup_bucket(hash, bucket) || character.nextInventorySerial == 0
        || quantity >= (std::numeric_limits<std::int32_t>::max)()
                           - static_cast<std::int64_t>(character.nextInventorySerial))
        return false;
    std::size_t available{};
    if (!pickup_space(character, bucket, available)) return false;
    if (static_cast<std::size_t>(quantity) > available) {
        (void)evict_oldest_stacks(
            character, bucket, static_cast<std::size_t>(quantity) - available);
        if (!pickup_space(character, bucket, available)) return false;
    }
    if (static_cast<std::size_t>(quantity) > available) return false;
    for (std::int32_t i = 0; i < quantity; ++i) {
        const auto slot = character.stacks.count++;
        lastSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        character.stacks.values[slot] = {hash, 1, lastSerial};
    }
    return true;
}

bool same_pickups(const CharacterState& expected, const CharacterState& after) noexcept {
    const auto next = [](const CharacterState& character,
                         std::size_t& i) -> const account::inventory::CharacterStack* {
        while (i < character.stacks.count) {
            const auto& row = character.stacks.values[i++];
            const auto ingredient = identity::ingredient(row.definitionHash);
            if (ingredient < identity::kIngredientCount
                && row.definitionHash == identity::kIngredients[ingredient].pickupHash)
                return &row;
        }
        return nullptr;
    };
    std::size_t a = 0, b = 0;
    for (;;) {
        const auto* left = next(expected, a);
        const auto* right = next(after, b);
        if (!left || !right) return left == right;
        if (left->definitionHash != right->definitionHash || left->quantity != right->quantity
            || left->mutationSerial != right->mutationSerial)
            return false;
    }
}
} // namespace

MaterialReward stage_reward(CharacterState&,
                            const DirectRecordReward& request,
                            PendingRecordRewardGrant& mutation,
                            PreparedRecordReward& result) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(request.itemDefinitionIndex, item))
        return MaterialReward::refused;
    const auto index = identity::ingredient(item.definitionHash);
    if (index == identity::kIngredientCount) return MaterialReward::none;
    if (!mutation.beforeDawning) {
        State before{};
        if (!read(before)) return MaterialReward::refused;
        mutation.beforeDawning = before;
        mutation.afterDawning = before;
    }
    std::int32_t credited{};
    if (!mutation.afterDawning
        || !credit(*mutation.afterDawning, item.definitionHash, request.quantity, credited))
        return MaterialReward::refused;
    result = {};
    result.definitionHash = identity::kIngredients[index].pickupHash;
    result.stateIndex = index;
    result.quantity = credited;
    result.afterQuantity = mutation.afterDawning->ingredients[index];
    result.kind = RecordRewardKind::accountMaterial;
    // The balance and durable acquisition queue commit with the bounty. Pickup publication
    // waits for the FIFO to drain instead of evicting feedback the client has not read yet.
    buckets::Descriptor bucket{};
    if (credited > 0 && !pickup_bucket(result.definitionHash, bucket))
        return MaterialReward::refused;
    return MaterialReward::staged;
}

bool validate_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    if (mutation.rewardCount > mutation.rewards.size()
        || mutation.beforeDawning.has_value() != mutation.afterDawning.has_value())
        return false;
    State expected{};
    if (mutation.beforeDawning && (!read(expected) || expected != *mutation.beforeDawning))
        return false;
    bool material = false;
    auto expectedCharacter = std::unique_ptr<CharacterState>{
        mutation.beforeDawning ? new (std::nothrow) CharacterState(mutation.beforeCharacter)
                               : nullptr};
    if (mutation.beforeDawning && !expectedCharacter) return false;
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        const auto index = identity::ingredient(reward.definitionHash);
        if (reward.kind != RecordRewardKind::accountMaterial) {
            // Ingredient acquisition must include its authoritative balance effect.
            if (index != identity::kIngredientCount) return false;
            continue;
        }
        material = true;
        if (!mutation.beforeDawning || index == identity::kIngredientCount
            || reward.stateIndex != index || reward.instanceSoid != 0 || reward.inventoryRow != 0
            || reward.definitionHash != identity::kIngredients[index].pickupHash
            || reward.appendedProfileResident || reward.quantity < 0)
            return false;
        std::int32_t credited{};
        // A full counter is an accepted zero-credit reward; no pickup is announced for it.
        if (!credit(expected, reward.definitionHash, (std::max)(1, reward.quantity), credited)
            || credited != reward.quantity || expected.ingredients[index] != reward.afterQuantity)
            return false;
        if (reward.mutationSerial != 0) return false;
    }
    return material == mutation.beforeDawning.has_value()
           && (!material
               || (expected == *mutation.afterDawning
                   && same_pickups(*expectedCharacter, mutation.afterCharacter)));
}

bool write_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    if (!validate_rewards(mutation)) return false;
    if (!mutation.beforeDawning) return true;
    if (!write(*mutation.beforeDawning, *mutation.afterDawning)) return false;
    investment::store::Statement queued(
        "INSERT INTO dawning_pickup_queue(character_soid,definition_hash,quantity) VALUES(?,?,?)");
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        if (reward.kind == RecordRewardKind::accountMaterial && reward.quantity > 0
            && !queued.write(mutation.characterSoid, reward.definitionHash, reward.quantity))
            return false;
    }
    return true;
}

bool stage_queued_pickups(std::uint64_t characterSoid, std::size_t& acquired) noexcept {
    namespace store = investment::store;
    acquired = 0;
    store::Transaction transaction;
    auto current = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    if (!transaction.ready() || !current || !store::read_account(*current)
        || !account::valid(*current))
        return false;
    CharacterState* character = nullptr;
    for (std::size_t i = 0; i < current->characterCount; ++i)
        if (current->characters[i].soid == characterSoid) character = &current->characters[i];
    if (!character || !character->selected) return false;
    for (std::size_t i = 0; i < character->stacks.count; ++i) {
        const auto& row = character->stacks.values[i];
        const auto index = identity::ingredient(row.definitionHash);
        if (index < identity::kIngredientCount
            && row.definitionHash == identity::kIngredients[index].pickupHash)
            return true; // An empty-row revision must precede reuse of these FIFO slots.
    }
    std::size_t capacity = character->stacks.values.size() - character->stacks.count;
    while (acquired < capacity) {
        std::uint64_t id{};
        std::uint32_t hash{};
        std::int32_t quantity{};
        {
            store::Statement row("SELECT id,definition_hash,quantity FROM dawning_pickup_queue "
                                 "WHERE character_soid=? ORDER BY id LIMIT 1");
            if (!row.parameters(characterSoid)) return false;
            const auto status = row.step();
            if (status == SQLITE_DONE) break;
            if (status != SQLITE_ROW || !row.columns(id, hash, quantity) || quantity <= 0)
                return false;
        }
        buckets::Descriptor bucket{};
        if (!pickup_bucket(hash, bucket)) return false;
        std::size_t available{};
        if (!pickup_space(*character, bucket, available)) return false;
        capacity = acquired + available;
        if (available == 0) break;
        const auto count = (std::min)(quantity, static_cast<std::int32_t>(capacity - acquired));
        std::int32_t serial{};
        if (!insert_pickups(*character, hash, count, serial)) return false;
        if (count == quantity) {
            store::Statement erase(
                "DELETE FROM dawning_pickup_queue WHERE id=? AND character_soid=?");
            if (!erase.write(id, characterSoid) || sqlite3_changes(store::g_database) != 1)
                return false;
        } else {
            store::Statement reduce(
                "UPDATE dawning_pickup_queue SET quantity=? WHERE id=? AND character_soid=?");
            if (!reduce.write(quantity - count, id, characterSoid)
                || sqlite3_changes(store::g_database) != 1)
                return false;
        }
        acquired += count;
    }
    if (acquired == 0) return transaction.commit();
    return account::valid(*current) && store::write_account(*current) && transaction.commit();
}

bool drain_pickups(std::uint64_t characterSoid, std::size_t& removed) noexcept {
    namespace store = investment::store;
    removed = 0;
    store::Transaction transaction;
    auto current = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    if (!transaction.ready() || !current || !store::read_account(*current)
        || !account::valid(*current))
        return false;
    CharacterState* character = nullptr;
    for (std::size_t i = 0; i < current->characterCount; ++i)
        if (current->characters[i].soid == characterSoid) character = &current->characters[i];
    if (!character || !character->selected) return false;
    auto& rows = character->stacks;
    std::size_t retained = 0;
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto& row = rows.values[i];
        buckets::Descriptor bucket{};
        if (pickup_bucket(row.definitionHash, bucket) && row.quantity == 1) {
            ++removed;
        } else {
            rows.values[retained++] = row;
        }
    }
    if (removed == 0) return transaction.commit();
    std::fill(
        rows.values.begin() + retained, rows.values.end(), account::inventory::CharacterStack{});
    rows.count = retained;
    return account::valid(*current) && store::write_account(*current) && transaction.commit();
}
} // namespace sunrise::state::runtime::detail::dawning
