#include "bucket_admission.h"

#include <algorithm>

#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../../middleware/datagen/family4/loadout/loadout_item_resolver.h"
#include "../build_data/runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {
namespace authored_inventory = account::inventory;
namespace family4_loadout = middleware::datagen::family4::loadout;
namespace inventory_buckets = build_data::inventory::buckets;
namespace item_details = build_data::items::details;

namespace {

/** Placement overrides the authored bucket only for Lost Items. */
[[nodiscard]] bool occupied_bucket(std::uint32_t definitionHash,
                                   authored_inventory::ItemPlacement placement,
                                   std::uint8_t& bucketId) noexcept {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(definitionHash, definition)) {
        return false;
    }
    bucketId = placement == authored_inventory::ItemPlacement::lostItems
                   ? inventory_buckets::kLostItemsBucketId
                   : definition.bucketId;
    return true;
}

/** @return The placement a row takes in this bucket; only Lost Items differs from the authored one.
 */
[[nodiscard]] authored_inventory::ItemPlacement
placement_in(const inventory_buckets::Descriptor& bucket) noexcept {
    return bucket.bucketId == inventory_buckets::kLostItemsBucketId
               ? authored_inventory::ItemPlacement::lostItems
               : authored_inventory::ItemPlacement::inventory;
}

[[nodiscard]] bool instanced(const item_details::Definition& detail) noexcept {
    return detail.instancedDefinitionState == item_details::InstancedDefinitionState::instanced;
}

/** Records one created, grown or moved row; the change rings bound the batch. */
[[nodiscard]] bool record(PlacedRows& rows,
                          GrantedRowArray array,
                          std::int32_t mutationSerial,
                          const char*& reason) noexcept {
    if (rows.count >= rows.values.size()) {
        reason = "change_capacity";
        return false;
    }
    rows.values[rows.count++] = {array, mutationSerial};
    return true;
}

[[nodiscard]] bool next_character_serial(CharacterState& character,
                                         std::int32_t& mutationSerial) noexcept {
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    return true;
}

/** Profile rows take the serial after the greatest one the profile holds. */
[[nodiscard]] bool next_profile_serial(const AccountState& account,
                                       std::int32_t& mutationSerial) noexcept {
    std::int32_t greatest = 0;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        greatest = (std::max)(greatest, account.profileItems[index].mutationSerial);
    }
    if (greatest == (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    mutationSerial = greatest + 1;
    return true;
}

/** Counts one bucket's rows in whichever saved array it belongs to. */
[[nodiscard]] bool bucket_occupancy(const AccountState& account,
                                    std::size_t characterIndex,
                                    const inventory_buckets::Descriptor& bucket,
                                    BucketAdmission& occupancy) noexcept {
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        return profile_bucket_admission(
            std::span(account.profileItems).first(account.profileItemCount),
            bucket.bucketId,
            occupancy);
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::character
           && character_bucket_admission(account.characters[characterIndex], bucket, occupancy);
}

/** @return Room the same item's stacks in this bucket still have. */
[[nodiscard]] std::int64_t stack_headroom(const AccountState& account,
                                          std::size_t characterIndex,
                                          const inventory_buckets::Descriptor& bucket,
                                          std::uint32_t definitionHash,
                                          std::int32_t maxStackSize) noexcept {
    std::int64_t headroom = 0;
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const auto& row = account.profileItems[index];
            if (row.definitionHash == definitionHash && row.quantity < maxStackSize) {
                headroom += maxStackSize - row.quantity;
            }
        }
        return headroom;
    }
    const auto placement = placement_in(bucket);
    const auto& stacks = account.characters[characterIndex].stacks;
    for (std::size_t index = 0; index < stacks.count; ++index) {
        const auto& row = stacks.values[index];
        if (row.definitionHash == definitionHash && row.placement == placement
            && row.quantity < maxStackSize) {
            headroom += maxStackSize - row.quantity;
        }
    }
    return headroom;
}

/** One saved row as the fit rules see it: its item, quantity and the bucket it occupies. */
struct HeldRow {
    build_data::items::Definition item{};
    std::int32_t quantity{};
    std::uint8_t bucketId{};
};

/**
 * Visits every row of the array a bucket belongs to, equipped rows first, in saved order.
 * @param visit Returns false to stop the walk.
 * @return False when a saved row names no installed item.
 */
template <typename Visit>
[[nodiscard]] bool visit_array(const AccountState& account,
                               std::size_t characterIndex,
                               const inventory_buckets::Descriptor& bucket,
                               Visit&& visit) noexcept {
    const auto row = [](std::uint32_t definitionHash,
                        std::int32_t quantity,
                        authored_inventory::ItemPlacement placement,
                        HeldRow& held) noexcept {
        held.quantity = quantity;
        return build_data::find_item_definition_hash(definitionHash, held.item)
               && occupied_bucket(definitionHash, placement, held.bucketId);
    };
    HeldRow held{};
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const auto& item = account.profileItems[index];
            if (!row(item.definitionHash,
                     item.quantity,
                     authored_inventory::ItemPlacement::inventory,
                     held)) {
                return false;
            }
            if (!visit(held)) {
                return true;
            }
        }
        return true;
    }
    const CharacterState& character = account.characters[characterIndex];
    for (const auto& equipped : character.equipment.slots) {
        if (equipped && !row(equipped->definitionHash, 1, equipped->placement, held)) {
            return false;
        }
        if (equipped && !visit(held)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const auto& item = character.inventory.values[index];
        if (!row(item.definitionHash, item.quantity, item.placement, held)) {
            return false;
        }
        if (!visit(held)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& item = character.stacks.values[index];
        if (!row(item.definitionHash, item.quantity, item.placement, held)) {
            return false;
        }
        if (!visit(held)) {
            return true;
        }
    }
    return true;
}

/** How the client's questline rule treats one arrival in one bucket. */
enum class QuestlineFit : std::uint8_t { unaffected, supersedes, blocked };

/**
 * An ordered questline admits only a later step than the one the bucket holds, which replaces it;
 * an equal or later held step, or an arrival missing from its own list, admits nothing.
 */
[[nodiscard]] bool questline_fit(const AccountState& account,
                                 std::size_t characterIndex,
                                 const inventory_buckets::Descriptor& bucket,
                                 const build_data::items::Definition& item,
                                 QuestlineFit& fit) noexcept {
    fit = QuestlineFit::unaffected;
    const auto& line = item.questline;
    if (line.questlineIndex == build_data::items::kUnavailableQuestlineIndex || !line.ordered) {
        return true;
    }
    if (line.step == build_data::items::kUnavailableQuestlineStep) {
        fit = QuestlineFit::blocked;
        return true;
    }
    bool held = false;
    std::uint16_t highest = 0;
    if (!visit_array(account, characterIndex, bucket, [&](const HeldRow& row) noexcept {
            if (row.bucketId == bucket.bucketId
                && row.item.questline.questlineIndex == line.questlineIndex
                && row.item.questline.step != build_data::items::kUnavailableQuestlineStep) {
                held = true;
                highest = (std::max)(highest, row.item.questline.step);
            }
            return true;
        })) {
        return false;
    }
    if (held) {
        fit = line.step > highest ? QuestlineFit::supersedes : QuestlineFit::blocked;
    }
    return true;
}

/**
 * A labelled item never shares its label with another row. The first row in the destination array
 * carrying the label decides: the same stackable item there takes what its stack has room for, any
 * other row admits nothing. With no such row, one new row is admitted if the bucket can take it.
 */
[[nodiscard]] bool label_fit(const AccountState& account,
                             std::size_t characterIndex,
                             const inventory_buckets::Descriptor& bucket,
                             const build_data::items::Definition& item,
                             const item_details::Definition& detail,
                             std::int32_t quantity,
                             bool roomForRow,
                             std::int32_t& admitted) noexcept {
    admitted = 0;
    bool decided = false;
    if (!visit_array(account, characterIndex, bucket, [&](const HeldRow& row) noexcept {
            if (row.item.stackUniqueLabel != item.stackUniqueLabel) {
                return true;
            }
            decided = true;
            if (row.item.definitionHash == item.definitionHash && !instanced(detail)
                && row.bucketId == bucket.bucketId && row.quantity < detail.maxStackSize) {
                admitted = (std::min)(quantity, detail.maxStackSize - row.quantity);
            }
            return false;
        })) {
        return false;
    }
    if (!decided && roomForRow) {
        admitted = (std::min)(quantity, instanced(detail) ? 1 : detail.maxStackSize);
    }
    return true;
}

/** Removes the earlier steps of an arrival's questline from the bucket it supersedes them in. */
[[nodiscard]] bool remove_superseded_steps(AccountState& working,
                                           std::size_t characterIndex,
                                           const inventory_buckets::Descriptor& bucket,
                                           const build_data::items::Definition& item,
                                           const char*& reason) noexcept {
    reason = "questline_step";
    const auto superseded = [&](std::uint32_t definitionHash,
                                authored_inventory::ItemPlacement placement,
                                bool& matches) noexcept {
        build_data::items::Definition held{};
        std::uint8_t bucketId = 0;
        if (!build_data::find_item_definition_hash(definitionHash, held)
            || !occupied_bucket(definitionHash, placement, bucketId)) {
            return false;
        }
        matches = bucketId == bucket.bucketId
                  && held.questline.questlineIndex == item.questline.questlineIndex
                  && held.questline.step < item.questline.step;
        return true;
    };
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = working.profileItemCount; index-- > 0;) {
            bool matches = false;
            if (!superseded(working.profileItems[index].definitionHash,
                            authored_inventory::ItemPlacement::inventory,
                            matches)
                || (matches && working.profileItems[index].instanceSoid != 0)) {
                return false;
            }
            if (matches) {
                auto& items = working.profileItems;
                std::move(items.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                          items.begin() + static_cast<std::ptrdiff_t>(working.profileItemCount),
                          items.begin() + static_cast<std::ptrdiff_t>(index));
                items[--working.profileItemCount] = {};
            }
        }
        return true;
    }
    CharacterState& character = working.characters[characterIndex];
    for (std::size_t index = character.stacks.count; index-- > 0;) {
        bool matches = false;
        const auto& held = character.stacks.values[index];
        if (!superseded(held.definitionHash, held.placement, matches)
            || (matches
                && !erase_character_bucket_row(character, kCharacterStackAdmissionBase + index))) {
            return false;
        }
    }
    for (std::size_t index = character.inventory.count; index-- > 0;) {
        bool matches = false;
        const auto& held = character.inventory.values[index];
        if (!superseded(held.definitionHash, held.placement, matches)
            || (matches && !erase_character_bucket_row(character, index))) {
            return false;
        }
    }
    return true;
}

/**
 * How much of an arrival one bucket admits: everything for a FIFO bucket, otherwise its free rows
 * at a whole stack each plus the room left on the item's own stacks there.
 * @param scanLabel False for a claim within one array, which the whole-row rule covers instead.
 */
[[nodiscard]] bool admitted_quantity(const AccountState& account,
                                     std::size_t characterIndex,
                                     const inventory_buckets::Descriptor& bucket,
                                     const build_data::items::Definition& item,
                                     const item_details::Definition& detail,
                                     std::int32_t quantity,
                                     bool scanLabel,
                                     std::int32_t& admitted) noexcept {
    admitted = 0;
    BucketAdmission occupancy;
    QuestlineFit questline = QuestlineFit::unaffected;
    if (!bucket_occupancy(account, characterIndex, bucket, occupancy)
        || !questline_fit(account, characterIndex, bucket, item, questline)) {
        return false;
    }
    if (questline == QuestlineFit::blocked) {
        return true;
    }
    // The step an arrival supersedes leaves when it lands, so its row counts as free.
    const std::size_t occupied = occupancy.count - (questline == QuestlineFit::supersedes ? 1 : 0);
    const std::size_t freeRows = occupied < bucket.slotCount ? bucket.slotCount - occupied : 0;
    const bool fifo = (bucket.policyFlags & inventory_buckets::kFifo) != 0;
    if (scanLabel && item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel) {
        return label_fit(account,
                         characterIndex,
                         bucket,
                         item,
                         detail,
                         quantity,
                         fifo || freeRows != 0,
                         admitted);
    }
    if (fifo) {
        admitted = quantity;
        return true;
    }
    if (instanced(detail)) {
        admitted = freeRows != 0 ? quantity : 0;
        return true;
    }
    const std::int64_t room =
        static_cast<std::int64_t>(freeRows) * detail.maxStackSize
        + stack_headroom(account, characterIndex, bucket, item.definitionHash, detail.maxStackSize);
    admitted = static_cast<std::int32_t>((std::min)(static_cast<std::int64_t>(quantity), room));
    return true;
}

[[nodiscard]] bool insert_stack(AccountState& working,
                                std::size_t characterIndex,
                                const inventory_buckets::Descriptor& bucket,
                                const build_data::items::Definition& item,
                                const item_details::Definition& detail,
                                std::int32_t quantity,
                                PlacedRows& rows,
                                const char*& reason) noexcept;

[[nodiscard]] bool insert_instance(AccountState& working,
                                   std::size_t characterIndex,
                                   const inventory_buckets::Descriptor& bucket,
                                   authored_inventory::Item item,
                                   PlacedRows& rows,
                                   const char*& reason) noexcept;

/**
 * Whether a full FIFO bucket's victim moves to its recovery bucket. The client checks the bucket's
 * no-transfer bit, a recovery bucket other than this one in the same array, the restricted list,
 * the stack-unique label, and room there. The victim is still in the array when its label is
 * checked, so a labelled victim always finds itself and is discarded. Lost Items is the only
 * recovery bucket a saved row can occupy, and it is FIFO, so it always has room.
 */
[[nodiscard]] bool transfers_victim(const inventory_buckets::Descriptor& bucket,
                                    const build_data::items::Definition& victim,
                                    inventory_buckets::Descriptor& recovery) noexcept {
    return (bucket.policyFlags & inventory_buckets::kNoTransferOnEviction) == 0
           && !victim.grantRestricted
           && victim.stackUniqueLabel == build_data::items::kNoStackUniqueLabel
           && victim.recoveryBucketId == inventory_buckets::kLostItemsBucketId
           && victim.recoveryBucketId != bucket.bucketId
           && family4_loadout::resolve_lost_items_bucket(recovery)
           && recovery.arraySelector == bucket.arraySelector;
}

/** Evicts the oldest unequipped row of a full character FIFO bucket, moving it when allowed. */
[[nodiscard]] bool evict_character_row(AccountState& working,
                                       std::size_t characterIndex,
                                       const inventory_buckets::Descriptor& bucket,
                                       PlacedRows& rows,
                                       const char*& reason) noexcept {
    CharacterState& character = working.characters[characterIndex];
    BucketAdmission occupancy;
    reason = "fifo_victim";
    if (!character_bucket_admission(character, bucket, occupancy)
        || occupancy.oldest >= kCharacterAdmissionCapacity) {
        return false;
    }
    const bool victimInstance = occupancy.oldest < kCharacterStackAdmissionBase;
    authored_inventory::Item instance{};
    authored_inventory::CharacterStack stack{};
    if (victimInstance) {
        instance = character.inventory.values[occupancy.oldest];
    } else {
        stack = character.stacks.values[occupancy.oldest - kCharacterStackAdmissionBase];
    }
    build_data::items::Definition victim{};
    item_details::Definition victimDetail{};
    if (!build_data::find_item_definition_hash(
            victimInstance ? instance.definitionHash : stack.definitionHash, victim)
        || !erase_character_bucket_row(character, occupancy.oldest)) {
        return false;
    }
    inventory_buckets::Descriptor recovery{};
    if (!transfers_victim(bucket, victim, recovery)) {
        return true;
    }
    if (victimInstance) {
        return insert_instance(working, characterIndex, recovery, instance, rows, reason);
    }
    return build_data::find_configured_item_detail(victim.definitionIndex, victimDetail)
           && insert_stack(working,
                           characterIndex,
                           recovery,
                           victim,
                           victimDetail,
                           stack.quantity,
                           rows,
                           reason);
}

/** Evicts the oldest row of a full profile FIFO bucket; Lost Items is a character bucket. */
[[nodiscard]] bool evict_profile_row(AccountState& working,
                                     const inventory_buckets::Descriptor& bucket,
                                     const char*& reason) noexcept {
    BucketAdmission occupancy;
    reason = "fifo_victim";
    if (!profile_bucket_admission(std::span(working.profileItems).first(working.profileItemCount),
                                  bucket.bucketId,
                                  occupancy)
        || occupancy.oldest >= working.profileItemCount) {
        return false;
    }
    // An action-source row is a published resident; no shipped FIFO profile bucket holds one.
    reason = "fifo_resident";
    if (working.profileItems[occupancy.oldest].instanceSoid != 0) {
        return false;
    }
    auto& items = working.profileItems;
    std::move(items.begin() + static_cast<std::ptrdiff_t>(occupancy.oldest) + 1,
              items.begin() + static_cast<std::ptrdiff_t>(working.profileItemCount),
              items.begin() + static_cast<std::ptrdiff_t>(occupancy.oldest));
    items[--working.profileItemCount] = {};
    return true;
}

/** Makes one free row in a bucket, evicting under FIFO policy; an ordinary bucket refuses. */
[[nodiscard]] bool free_row(AccountState& working,
                            std::size_t characterIndex,
                            const inventory_buckets::Descriptor& bucket,
                            PlacedRows& rows,
                            const char*& reason) noexcept {
    BucketAdmission occupancy;
    reason = "bucket_full";
    if (!bucket_occupancy(working, characterIndex, bucket, occupancy)) {
        return false;
    }
    if (occupancy.count < bucket.slotCount) {
        return true;
    }
    if ((bucket.policyFlags & inventory_buckets::kFifo) == 0) {
        return false;
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::profile
               ? evict_profile_row(working, bucket, reason)
               : evict_character_row(working, characterIndex, bucket, rows, reason);
}

/** Tops up the item's own stacks in the bucket, then adds whole new stacks. */
bool insert_stack(AccountState& working,
                  std::size_t characterIndex,
                  const inventory_buckets::Descriptor& bucket,
                  const build_data::items::Definition& item,
                  const item_details::Definition& detail,
                  std::int32_t quantity,
                  PlacedRows& rows,
                  const char*& reason) noexcept {
    reason = "stack_insert";
    if (quantity <= 0 || detail.maxStackSize <= 0 || instanced(detail)) {
        return false;
    }
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = 0; quantity > 0 && index < working.profileItemCount; ++index) {
            auto& row = working.profileItems[index];
            if (row.definitionHash != item.definitionHash || row.quantity >= detail.maxStackSize) {
                continue;
            }
            const std::int32_t added = (std::min)(quantity, detail.maxStackSize - row.quantity);
            std::int32_t serial = 0;
            if (!next_profile_serial(working, serial)
                || !record(rows, GrantedRowArray::profile, serial, reason)) {
                return false;
            }
            row.quantity += added;
            row.mutationSerial = serial;
            quantity -= added;
        }
        while (quantity > 0) {
            std::int32_t serial = 0;
            std::uint64_t soid = 0;
            if (!free_row(working, characterIndex, bucket, rows, reason)
                || working.profileItemCount >= working.profileItems.size()
                || !next_profile_serial(working, serial)
                || (build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
                    && !next_profile_item_instance_soid(working, soid))
                || !record(rows, GrantedRowArray::profile, serial, reason)) {
                return false;
            }
            const std::int32_t added = (std::min)(quantity, detail.maxStackSize);
            working.profileItems[working.profileItemCount++] = {
                soid, item.definitionHash, added, serial};
            quantity -= added;
        }
        return true;
    }
    if (bucket.arraySelector != inventory_buckets::ArraySelector::character) {
        return false;
    }
    const auto placement = placement_in(bucket);
    for (std::size_t index = 0;; ++index) {
        CharacterState& character = working.characters[characterIndex];
        if (quantity == 0 || index >= character.stacks.count) {
            break;
        }
        auto& row = character.stacks.values[index];
        if (row.definitionHash != item.definitionHash || row.placement != placement
            || row.quantity >= detail.maxStackSize) {
            continue;
        }
        const std::int32_t added = (std::min)(quantity, detail.maxStackSize - row.quantity);
        std::int32_t serial = 0;
        if (!next_character_serial(character, serial)
            || !record(rows, GrantedRowArray::characterStack, serial, reason)) {
            return false;
        }
        row.quantity += added;
        row.mutationSerial = serial;
        quantity -= added;
    }
    while (quantity > 0) {
        if (!free_row(working, characterIndex, bucket, rows, reason)) {
            return false;
        }
        CharacterState& character = working.characters[characterIndex];
        std::int32_t serial = 0;
        if (character.stacks.count >= character.stacks.values.size()
            || !next_character_serial(character, serial)
            || !record(rows, GrantedRowArray::characterStack, serial, reason)) {
            return false;
        }
        const std::int32_t added = (std::min)(quantity, detail.maxStackSize);
        character.stacks.values[character.stacks.count++] = {
            item.definitionHash, added, serial, placement};
        quantity -= added;
    }
    return true;
}

/** Adds one instance, keeping its identity, in a free or freed row of a character bucket. */
bool insert_instance(AccountState& working,
                     std::size_t characterIndex,
                     const inventory_buckets::Descriptor& bucket,
                     authored_inventory::Item item,
                     PlacedRows& rows,
                     const char*& reason) noexcept {
    reason = "instance_insert";
    if (bucket.arraySelector != inventory_buckets::ArraySelector::character
        || !free_row(working, characterIndex, bucket, rows, reason)) {
        return false;
    }
    CharacterState& character = working.characters[characterIndex];
    std::int32_t serial = 0;
    if (character.inventory.count >= character.inventory.values.size()
        || !next_character_serial(character, serial)
        || !record(rows, GrantedRowArray::characterItem, serial, reason)) {
        return false;
    }
    item.placement = placement_in(bucket);
    item.mutationSerial = serial;
    character.inventory.values[character.inventory.count++] = item;
    return true;
}

/** Inserts what one bucket admitted, allocating the identity of a new instance first. */
[[nodiscard]] bool insert_admitted(AccountState& working,
                                   std::size_t characterIndex,
                                   const inventory_buckets::Descriptor& bucket,
                                   const Arrival& arrival,
                                   std::int32_t quantity,
                                   std::uint64_t& nextInstanceSoid,
                                   PlacedRows& rows,
                                   const char*& reason) noexcept {
    QuestlineFit questline = QuestlineFit::unaffected;
    if (!questline_fit(working, characterIndex, bucket, arrival.item, questline)
        || (questline == QuestlineFit::supersedes
            && !remove_superseded_steps(working, characterIndex, bucket, arrival.item, reason))) {
        return false;
    }
    if (!instanced(arrival.detail)) {
        return insert_stack(
            working, characterIndex, bucket, arrival.item, arrival.detail, quantity, rows, reason);
    }
    reason = "instance_identity";
    authored_inventory::Item instance = arrival.instance;
    if (!next_item_instance_soid(working, instance.instanceSoid, nextInstanceSoid)
        || instance.instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    nextInstanceSoid = instance.instanceSoid + 1;
    instance.definitionHash = arrival.item.definitionHash;
    instance.quantity = 1;
    return insert_instance(working, characterIndex, bucket, instance, rows, reason);
}

} // namespace

/** Counts one bucket's rows; equipped rows hold capacity but are never offered for eviction. */
bool character_bucket_admission(const CharacterState& character,
                                const inventory_buckets::Descriptor& bucket,
                                BucketAdmission& occupancy) noexcept {
    occupancy = {};
    if (character.inventory.count > character.inventory.values.size()
        || character.stacks.count > character.stacks.values.size()) {
        return false;
    }
    // Equipped rows count toward capacity but are never offered for eviction.
    for (const auto& equipped : character.equipment.slots) {
        std::uint8_t bucketId = 0;
        if (equipped && !occupied_bucket(equipped->definitionHash, equipped->placement, bucketId)) {
            return false;
        }
        occupancy.count += equipped && bucketId == bucket.bucketId;
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const auto& item = character.inventory.values[index];
        std::uint8_t bucketId = 0;
        if (!occupied_bucket(item.definitionHash, item.placement, bucketId)) {
            return false;
        }
        if (bucketId == bucket.bucketId) {
            occupancy.include(index, item.mutationSerial);
        }
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& item = character.stacks.values[index];
        std::uint8_t bucketId = 0;
        if (!occupied_bucket(item.definitionHash, item.placement, bucketId)) {
            return false;
        }
        if (bucketId == bucket.bucketId) {
            occupancy.include(kCharacterStackAdmissionBase + index, item.mutationSerial);
        }
    }
    return true;
}

/** Removes one held instance or stack named by its combined admission index. */
bool erase_character_bucket_row(CharacterState& character, std::size_t row) noexcept {
    if (row < kCharacterStackAdmissionBase) {
        auto& held = character.inventory;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    } else {
        auto& held = character.stacks;
        row -= kCharacterStackAdmissionBase;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    }
    return true;
}

/** Places what fits in the item's own bucket, then its recovery bucket, and drops the rest. */
bool place_arrival(AccountState& working,
                   std::size_t characterIndex,
                   const Arrival& arrival,
                   std::uint64_t& nextInstanceSoid,
                   PlacedRows& rows,
                   ArrivalOutcome& outcome,
                   const char*& reason) noexcept {
    outcome = {};
    reason = "arrival";
    inventory_buckets::Descriptor own{};
    if (characterIndex >= working.characterCount || arrival.quantity <= 0
        || (instanced(arrival.detail) && arrival.quantity != 1)
        || !build_data::find_inventory_bucket_descriptor(arrival.item.bucketId, own)) {
        return false;
    }
    // The client refuses a restricted item outright: neither its bucket nor its recovery gets any.
    if (arrival.item.grantRestricted) {
        outcome.dropped = arrival.quantity;
        reason = nullptr;
        return true;
    }
    std::int32_t remaining = arrival.quantity;
    std::int32_t admitted = 0;
    if (!admitted_quantity(
            working, characterIndex, own, arrival.item, arrival.detail, remaining, true, admitted)
        || (admitted != 0
            && !insert_admitted(
                working, characterIndex, own, arrival, admitted, nextInstanceSoid, rows, reason))) {
        return false;
    }
    remaining -= admitted;
    // Lost Items is the only recovery bucket a saved row can occupy; the shipped build names
    // another one only for 5 definitions, whose remainder is dropped like an absent recovery.
    inventory_buckets::Descriptor recovery{};
    if (remaining != 0 && arrival.item.recoveryBucketId == inventory_buckets::kLostItemsBucketId
        && own.bucketId != inventory_buckets::kLostItemsBucketId
        && family4_loadout::resolve_lost_items_bucket(recovery)) {
        if (!admitted_quantity(working,
                               characterIndex,
                               recovery,
                               arrival.item,
                               arrival.detail,
                               remaining,
                               true,
                               admitted)
            || (admitted != 0
                && !insert_admitted(working,
                                    characterIndex,
                                    recovery,
                                    arrival,
                                    admitted,
                                    nextInstanceSoid,
                                    rows,
                                    reason))) {
            return false;
        }
        remaining -= admitted;
    }
    outcome.placed = arrival.quantity - remaining;
    outcome.dropped = remaining;
    reason = nullptr;
    return true;
}

/** Validates and moves one Lost Items claim in the client's order: debit, then insert. */
bool place_claim(AccountState& working,
                 std::size_t characterIndex,
                 const build_data::items::Definition& item,
                 const item_details::Definition& detail,
                 std::uint64_t instanceSoid,
                 std::int32_t quantity,
                 PlacedRows& rows,
                 const char*& reason) noexcept {
    reason = "claim_item";
    inventory_buckets::Descriptor destination{};
    inventory_buckets::Descriptor lostItems{};
    if (characterIndex >= working.characterCount || quantity <= 0
        || item.recoveryBucketId != inventory_buckets::kLostItemsBucketId
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, destination)
        || destination.bucketId == inventory_buckets::kLostItemsBucketId
        || !family4_loadout::resolve_lost_items_bucket(lostItems)) {
        return false;
    }
    reason = "claim_restricted";
    if (item.grantRestricted) {
        return false;
    }
    CharacterState& character = working.characters[characterIndex];
    const bool instance = instanced(detail);
    const auto lost = authored_inventory::ItemPlacement::lostItems;
    reason = "claim_presence";
    std::int64_t held = 0;
    std::int32_t firstRow = 0;
    std::size_t instanceIndex = character.inventory.count;
    if (instance) {
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            const auto& row = character.inventory.values[index];
            if (row.instanceSoid == instanceSoid && row.placement == lost
                && row.definitionHash == item.definitionHash) {
                instanceIndex = index;
            }
        }
        if (instanceSoid == 0 || quantity != 1 || instanceIndex == character.inventory.count) {
            return false;
        }
    } else {
        for (std::size_t index = 0; index < character.stacks.count; ++index) {
            const auto& row = character.stacks.values[index];
            if (row.definitionHash == item.definitionHash && row.placement == lost) {
                firstRow = held == 0 ? row.quantity : firstRow;
                held += row.quantity;
            }
        }
        if (instanceSoid != 0 || held < quantity) {
            return false;
        }
    }
    // A labelled stack claimed within one array moves its first row whole, or not at all.
    const bool sameArray = destination.arraySelector == lostItems.arraySelector;
    reason = "claim_label";
    if (sameArray && !instance && item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel
        && firstRow != quantity) {
        return false;
    }
    reason = "claim_questline";
    QuestlineFit questline = QuestlineFit::unaffected;
    if (!questline_fit(working, characterIndex, destination, item, questline)
        || questline == QuestlineFit::blocked) {
        return false;
    }
    reason = "claim_capacity";
    std::int32_t admitted = 0;
    if (!admitted_quantity(
            working, characterIndex, destination, item, detail, quantity, !sameArray, admitted)
        || admitted < quantity) {
        return false;
    }
    // Lost Items is debited first, and with no change record.
    authored_inventory::Item moved{};
    if (instance) {
        moved = character.inventory.values[instanceIndex];
        if (!erase_character_bucket_row(character, instanceIndex)) {
            return false;
        }
    } else {
        std::int32_t owed = quantity;
        for (std::size_t index = character.stacks.count; owed > 0 && index-- > 0;) {
            auto& row = character.stacks.values[index];
            if (row.definitionHash != item.definitionHash || row.placement != lost) {
                continue;
            }
            const std::int32_t taken = (std::min)(owed, row.quantity);
            row.quantity -= taken;
            owed -= taken;
            if (row.quantity == 0
                && !erase_character_bucket_row(character, kCharacterStackAdmissionBase + index)) {
                return false;
            }
        }
    }
    if ((questline == QuestlineFit::supersedes
         && !remove_superseded_steps(working, characterIndex, destination, item, reason))
        || (instance ? !insert_instance(working, characterIndex, destination, moved, rows, reason)
                     : !insert_stack(working,
                                     characterIndex,
                                     destination,
                                     item,
                                     detail,
                                     quantity,
                                     rows,
                                     reason))) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** Counts announced rows against each change list's capacity. */
const char* change_ring_refusal(std::span<const GrantedRow> rows) noexcept {
    std::size_t characterChanges = 0;
    std::size_t profileChanges = 0;
    for (const auto& row : rows) {
        if (row.array == GrantedRowArray::profile) {
            if (++profileChanges > middleware::datagen::family4::account::layout::
                    kProfileInventoryChangeRecordCapacity) {
                return "profile_change_capacity";
            }
        } else if (++characterChanges > middleware::datagen::family4::character::layout::
                       kInventoryChangeRecordCapacity) {
            return "character_change_capacity";
        }
    }
    return nullptr;
}

/** Looks a granted row up by its serial in the array it names. */
bool row_present(const AccountState& account,
                 std::size_t characterIndex,
                 const GrantedRow& row) noexcept {
    const auto matches = [&](const auto& held) noexcept {
        return held.mutationSerial == row.mutationSerial;
    };
    if (row.array == GrantedRowArray::profile) {
        const auto held = std::span(account.profileItems).first(account.profileItemCount);
        return std::any_of(held.begin(), held.end(), matches);
    }
    const CharacterState& character = account.characters[characterIndex];
    if (row.array == GrantedRowArray::characterItem) {
        const auto held = std::span(character.inventory.values).first(character.inventory.count);
        return std::any_of(held.begin(), held.end(), matches);
    }
    const auto held = std::span(character.stacks.values).first(character.stacks.count);
    return std::any_of(held.begin(), held.end(), matches);
}

/** Copies the batch's images and keeps the rows still present after it. */
bool record_grant_images(const AccountState& before,
                         const AccountState& after,
                         std::size_t characterIndex,
                         const PlacedRows& placed,
                         PendingRecordRewardGrant& mutation,
                         const char*& reason) noexcept {
    // A row a later change or eviction in the batch replaced is not announced.
    mutation.rowCount = 0;
    for (std::size_t index = 0; index < placed.count; ++index) {
        if (row_present(after, characterIndex, placed.values[index])) {
            mutation.rows[mutation.rowCount++] = placed.values[index];
        }
    }
    reason = change_ring_refusal(std::span(mutation.rows).first(mutation.rowCount));
    if (reason != nullptr) {
        return false;
    }
    mutation.beforeCharacter = before.characters[characterIndex];
    mutation.afterCharacter = after.characters[characterIndex];
    mutation.beforeProfileItems = before.profileItems;
    mutation.afterProfileItems = after.profileItems;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = before.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.beforeProfileItemCount = before.profileItemCount;
    mutation.afterProfileItemCount = after.profileItemCount;
    mutation.prepared = true;
    return true;
}
} // namespace sunrise::state::runtime::detail
