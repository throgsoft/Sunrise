#include "postmaster_runtime.h"

#include <limits>
#include <memory>
#include <new>

#include "../../middleware/datagen/family4/loadout/loadout_item_resolver.h"
#include "../account/inventory/postmaster_policy.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "../investment/store_internal.h"
#include "character_encoding_preflight.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {
namespace inventory = account::inventory;
namespace loadout = middleware::datagen::family4::loadout;
namespace buckets = build_data::inventory::buckets;
using namespace runtime::detail;

bool accepted_claim_rule(std::uint16_t vendorIndex, std::uint8_t sourceBucket) noexcept {
    if (sourceBucket != 34 || (vendorIndex != 12 && vendorIndex != 13)) return false;
    const std::uint32_t hash = vendorIndex == 12 ? 3161908920U : 1846565192U;
    build_data::vendors::IndexEntry index{};
    build_data::vendors::Definition vendor{};
    if (!build_data::vendors::find_index(vendorIndex, index) || index.index != vendorIndex
        || index.definitionHash != hash || !build_data::vendors::find(hash, vendor)
        || vendor.index != vendorIndex || vendor.definitionHash != hash
        || vendor.definitionTag != index.definitionTag || !vendor.transferRulesAvailable
        || vendor.transferRuleCount > vendor.transferRules.size()) return false;
    for (std::size_t i = 0; i < vendor.transferRuleCount; ++i) {
        if (vendor.transferRules[i].sourceBucket == sourceBucket)
            return vendor.transferRules[i].destinationBucket
                   == build_data::vendors::kAuthoredDestination;
    }
    return false;
}

bool ordinary_item(const inventory::Item& item,
                   build_data::items::Definition& definition,
                   buckets::Descriptor& bucket) noexcept {
    build_data::items::details::Definition detail{};
    return build_data::find_item_definition_hash(item.definitionHash, definition)
           && definition.definitionHash == item.definitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == definition.definitionHash
           && detail.bucketId == definition.bucketId
           && inventory::postmaster_supported(item, detail)
           && build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
           && bucket.bucketId == definition.bucketId
           && bucket.arraySelector == buckets::ArraySelector::character
           && bucket.bucketId != 34 && bucket.bucketId != 37 && bucket.slotCount != 0;
}

struct ClaimScratch {
    AccountState after{};
    loadout::ResolvedLoadout beforeLoadout{};
    loadout::ResolvedLoadout afterLoadout{};
};

bool stage_claim(const AccountState& before,
                 std::uint16_t vendorIndex,
                 std::uint8_t sourceBucket,
                 std::uint64_t instanceSoid,
                 std::uint16_t definitionIndex,
                 PendingPostmasterClaim& mutation) noexcept {
    const auto characterIndex = selected_character_index(before);
    if (!account::valid(before) || !valid_profile_inventory(before)
        || characterIndex >= before.characterCount || instanceSoid == 0
        || !accepted_claim_rule(vendorIndex, sourceBucket)) return false;
    const auto& character = before.characters[characterIndex];
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) return false;
    std::size_t index = 0;
    while (index < character.inventory.count
           && character.inventory.values[index].instanceSoid != instanceSoid) ++index;
    if (index == character.inventory.count) return false;
    const auto& source = character.inventory.values[index];
    build_data::items::Definition definition{};
    buckets::Descriptor destination{}, postmaster{};
    if (source.placement != inventory::ItemPlacement::postmaster
        || !ordinary_item(source, definition, destination)
        || definition.definitionIndex != definitionIndex
        || !loadout::resolve_postmaster_bucket(postmaster)) return false;

    const std::unique_ptr<ClaimScratch> scratch(new (std::nothrow) ClaimScratch{});
    if (!scratch || !loadout::resolve(before, characterIndex, scratch->beforeLoadout)) return false;
    ResolvedPosition sourcePosition{};
    if (!find_resolved_position(scratch->beforeLoadout, instanceSoid, sourcePosition)
        || sourcePosition.equipped || sourcePosition.inventoryRow < postmaster.firstSlot
        || sourcePosition.inventoryRow >= postmaster.firstSlot + postmaster.slotCount) return false;
    scratch->after = before;
    auto& afterCharacter = scratch->after.characters[characterIndex];
    auto& claimed = afterCharacter.inventory.values[index];
    claimed.placement = inventory::ItemPlacement::inventory;
    claimed.mutationSerial = static_cast<std::int32_t>(afterCharacter.nextInventorySerial++);
    ResolvedPosition destinationPosition{};
    if (!account::valid(scratch->after)
        || !loadout::resolve(scratch->after, characterIndex, scratch->afterLoadout)
        || !find_resolved_position(scratch->afterLoadout, instanceSoid, destinationPosition)
        || destinationPosition.equipped || destinationPosition.inventoryRow < destination.firstSlot
        || destinationPosition.inventoryRow >= destination.firstSlot + destination.slotCount
        || !character_encoding_preflight(scratch->after, characterIndex, scratch->afterLoadout))
        return false;

    mutation.beforeCharacter = character;
    mutation.afterCharacter = afterCharacter;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = character.soid;
    mutation.instanceSoid = instanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.inventoryIndex = index;
    mutation.expectedNextInventorySerial = character.nextInventorySerial;
    mutation.quantity = 1;
    mutation.vendorIndex = vendorIndex;
    mutation.definitionIndex = definitionIndex;
    mutation.sourceBucket = sourceBucket;
    mutation.beforeInventoryRow = sourcePosition.inventoryRow;
    mutation.afterInventoryRow = destinationPosition.inventoryRow;
    mutation.prepared = true;
    return true;
}

bool materialize_claim(const AccountState& current,
                       const PendingPostmasterClaim& mutation,
                       AccountState& after) noexcept {
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.quantity != 1
        || current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || current.characters[mutation.characterIndex].soid != mutation.characterSoid
        || current.characters[mutation.characterIndex].nextInventorySerial
               != mutation.expectedNextInventorySerial
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter))
        return false;
    const std::unique_ptr<PendingPostmasterClaim> canonical(
        new (std::nothrow) PendingPostmasterClaim{});
    if (!canonical || !stage_claim(current, mutation.vendorIndex, mutation.sourceBucket,
                                    mutation.instanceSoid, mutation.definitionIndex, *canonical)
        || canonical->characterIndex != mutation.characterIndex
        || canonical->inventoryIndex != mutation.inventoryIndex
        || canonical->beforeInventoryRow != mutation.beforeInventoryRow
        || canonical->afterInventoryRow != mutation.afterInventoryRow
        || !same_character(canonical->afterCharacter, mutation.afterCharacter)) return false;
    after = current;
    after.characters[mutation.characterIndex] = canonical->afterCharacter;
    return true;
}
} // namespace

bool prepare_postmaster_claim(std::uint16_t vendorIndex, std::uint8_t sourceBucket,
                              std::uint64_t instanceSoid, std::uint16_t definitionIndex,
                              std::int32_t quantity, PendingPostmasterClaim& mutation) noexcept {
    std::destroy_at(&mutation);
    std::construct_at(&mutation);
    if (quantity != 1) return false;
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    return before && investment::store::read_account(*before)
           && stage_claim(*before, vendorIndex, sourceBucket, instanceSoid, definitionIndex, mutation);
}

bool preview_postmaster_claim(const PendingPostmasterClaim& mutation, AccountState& after) noexcept {
    std::destroy_at(&after);
    std::construct_at(&after);
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    return before && investment::store::read_account(*before)
           && materialize_claim(*before, mutation, after);
}

bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept {
    const runtime::detail::PendingConsumption consume{mutation};
    investment::store::Transaction transaction;
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    const std::unique_ptr<AccountState> after(new (std::nothrow) AccountState{});
    return transaction.ready() && before && after && investment::store::read_account(*before)
           && materialize_claim(*before, mutation, *after)
           && investment::store::write_account(*after) && transaction.commit();
}

namespace runtime::detail {
bool place_instanced_reward(const AccountState& before, std::size_t characterIndex,
                            inventory::Item& item) noexcept {
    if (!account::valid(before) || characterIndex >= before.characterCount
        || item.placement != inventory::ItemPlacement::inventory) return false;
    struct Scratch { loadout::ResolvedLoadout resolved{}; loadout::Candidate item{}; };
    const std::unique_ptr<Scratch> scratch(new (std::nothrow) Scratch{});
    if (!scratch || !loadout::resolve(before, characterIndex, scratch->resolved)
        || !character_encoding_preflight(before, characterIndex, scratch->resolved, false)
        || !loadout::resolve_item(item, before.characters[characterIndex],
                                  build_data::item_definition_count(),
                                  build_data::socket_entry_list_count(), false, scratch->item))
        return false;
    const auto& bucket = scratch->item.bucket;
    std::size_t used = 0;
    for (std::size_t i = 0; i < scratch->resolved.itemCount; ++i) {
        const auto row = scratch->resolved.items[i].inventoryRow;
        used += row >= bucket.firstSlot && row < bucket.firstSlot + bucket.slotCount;
    }
    if (used < bucket.slotCount) return true;
    if (used != bucket.slotCount) return false;
    build_data::items::Definition definition{};
    buckets::Descriptor destination{}, postmaster{};
    if (!ordinary_item(item, definition, destination)
        || !loadout::resolve_postmaster_bucket(postmaster)) return false;
    std::size_t postmasterCount = 0;
    const auto& held = before.characters[characterIndex].inventory;
    for (std::size_t i = 0; i < held.count; ++i)
        postmasterCount += held.values[i].placement == inventory::ItemPlacement::postmaster;
    if (postmasterCount >= inventory::kPostmasterItemCapacity) return false;
    item.placement = inventory::ItemPlacement::postmaster;
    return true;
}
} // namespace runtime::detail
} // namespace sunrise::state
