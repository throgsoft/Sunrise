#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "eververse_runtime.h"

#include <memory>
#include <new>

#include "../build_data/eververse/manifest_catalog.h"
#include "character_encoding_preflight.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::eververse {
namespace store = investment::store;

bool read_ownership(std::uint64_t accountSoid, std::uint32_t definitionHash, bool& owned) noexcept {
    owned = false;
    if (accountSoid == 0 || definitionHash == 0) return false;
    store::Transaction transaction;
    if (!transaction.ready()) return false;
    store::Statement row(
        "SELECT 1 FROM cosmetic_acquisitions WHERE account_soid=? AND item_hash=?");
    if (!row.parameters(accountSoid, definitionHash)) return false;
    const int result = row.step();
    if (result == SQLITE_ROW) {
        owned = true;
        if (row.step() != SQLITE_DONE) return false;
    } else if (result != SQLITE_DONE) {
        return false;
    }
    return transaction.commit();
}

bool record_ownership(std::uint64_t accountSoid, std::uint32_t definitionHash) noexcept {
    if (accountSoid == 0 || definitionHash == 0) return false;
    store::Transaction transaction;
    if (!transaction.ready()) return false;
    store::Statement owner("SELECT soid FROM account WHERE id=1");
    std::uint64_t current{};
    build_data::items::Definition item{};
    if (owner.step() != SQLITE_ROW || !owner.column(0, current) || current != accountSoid
        || owner.step() != SQLITE_DONE
        || !build_data::find_item_definition_hash(definitionHash, item))
        return false;
    store::Statement row("INSERT INTO cosmetic_acquisitions(account_soid,item_hash) VALUES (?,?) "
                         "ON CONFLICT(account_soid,item_hash) DO NOTHING");
    return row.write(accountSoid, definitionHash) && transaction.commit();
}

bool prepare_native_ownership(std::uint16_t itemIndex,
                              bool activate,
                              NativeOwnership& mutation) noexcept {
    mutation = {};
    build_data::items::details::Definition detail{};
    if (!build_data::find_configured_item_detail(itemIndex, detail)
        || detail.definitionIndex != itemIndex || detail.acquiredFlagSlot == 0xFFFFU
        || detail.acquiredAccountFlag >= unlocks::kAccountFlagCapacity)
        return false;
    std::int32_t before{};
    if (!store::read_unlock(store::Bank::accountFlags, detail.acquiredAccountFlag, before)
        || before < 0 || before > unlocks::kFlagSet)
        return false;
    mutation = {itemIndex,
                detail.acquiredFlagSlot,
                detail.acquiredAccountFlag,
                static_cast<std::uint8_t>(before),
                activate ? unlocks::kFlagSet : static_cast<std::uint8_t>(before),
                true};
    return true;
}

bool write_native_ownership(const NativeOwnership& mutation) noexcept {
    if (!mutation.prepared) return mutation == NativeOwnership{};
    store::Transaction transaction;
    NativeOwnership canonical{};
    return transaction.ready()
           && prepare_native_ownership(
               mutation.itemIndex, mutation.after == unlocks::kFlagSet, canonical)
           && canonical == mutation
           && (mutation.before == mutation.after
               || store::write_unlock(
                   store::Bank::accountFlags, mutation.accountFlag, mutation.after))
           && transaction.commit();
}

namespace {
namespace helpers = runtime::detail;
namespace items = build_data::items;
namespace buckets = build_data::inventory::buckets;
namespace catalog = build_data::eververse;
namespace loadout = middleware::datagen::family4::loadout;

bool unlock_definition(std::uint16_t index, items::Definition& item) noexcept {
    items::details::Definition detail{};
    catalog::ItemMetadata metadata{};
    buckets::Descriptor bucket{};
    return build_data::find_item_definition_index(index, item) && item.definitionIndex == index
           && build_data::find_configured_item_detail(index, detail)
           && detail.definitionIndex == index && detail.definitionHash == item.definitionHash
           && detail.bucketId == item.bucketId && !detail.equipmentSlot
           && detail.objectiveCount == 0 && detail.rewardCount == 0
           && detail.instancedDefinitionState == items::details::InstancedDefinitionState::stackable
           && detail.acquiredFlagSlot != 0xFFFFU
           && detail.acquiredAccountFlag < unlocks::kAccountFlagCapacity
           && build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
           && bucket.bucketId == item.bucketId && bucket.arraySelector == buckets::ArraySelector::profile
           && catalog::read_item(item.definitionHash, metadata)
           && metadata.itemHash == item.definitionHash && metadata.itemIndex == index
           && metadata.bucketIndex == item.bucketId && metadata.maxStackSize == detail.maxStackSize
           && metadata.maxStackSize > 0 && !metadata.instanced && !metadata.isDummy
           && !metadata.isWrapper && metadata.previewVendorHash == 0
           && metadata.unlockAction && metadata.deleteOnAction && !metadata.useOnAcquire
           && !metadata.onActionRecreateSelf && metadata.plugCategoryHash != 0
           && catalog::is_simple_unlock_action(item.definitionHash);
}

std::size_t instance_occurrences(const AccountState& account, std::uint64_t soid) noexcept {
    std::size_t count{};
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        for (const auto& equipped : account.characters[c].equipment.slots)
            count += equipped && equipped->instanceSoid == soid;
        for (std::size_t i = 0; i < account.characters[c].inventory.count; ++i)
            count += account.characters[c].inventory.values[i].instanceSoid == soid;
    }
    for (std::size_t i = 0; i < account.profileItemCount; ++i)
        count += account.profileItems[i].instanceSoid == soid;
    return count;
}

bool stage_unlock(const AccountState& current,
                  bool hasInstance,
                  std::uint64_t instanceSoid,
                  std::uint16_t index,
                  std::int32_t expectedQuantity,
                  std::int8_t selector,
                  PendingRecordRewardGrant& mutation,
                  const char*& reason) noexcept {
    mutation = {};
    reason = "unlock_account_or_quantity";
    if (!account::valid(current) || !helpers::valid_profile_inventory(current)
        || hasInstance != (instanceSoid != 0) || expectedQuantity != 1)
        return false;
    std::size_t characterIndex = current.characterCount, selectedCount{};
    for (std::size_t c = 0; c < current.characterCount; ++c) {
        if (!current.characters[c].selected) continue;
        characterIndex = c;
        ++selectedCount;
    }
    reason = "unlock_selected_character";
    if (selectedCount != 1) return false;
    items::Definition item{};
    reason = "unsupported_unlock_action";
    if (!unlock_definition(index, item)) return false;
    // Native 52AA70 resolves -1 to definition+B8; other values select the source bucket.
    // The authored action supplies Unlock/delete semantics, not a distinct wire selector.
    reason = "unlock_bucket_selector";
    if (selector != -1 && (selector < 0 || static_cast<unsigned>(selector) != item.bucketId))
        return false;
    std::size_t sourceIndex = current.profileItemCount, matches{};
    for (std::size_t i = 0; i < current.profileItemCount; ++i) {
        if (current.profileItems[i].definitionHash != item.definitionHash) continue;
        sourceIndex = i;
        ++matches;
    }
    reason = "unlock_source_not_unique";
    if (matches != 1) return false;
    const auto& source = current.profileItems[sourceIndex];
    const bool resident = build_data::is_profile_action_source(index, item.bucketId);
    reason = "unlock_source_identity_or_quantity";
    if (source.quantity != 1 || resident != (source.instanceSoid != 0)
        || (hasInstance && source.instanceSoid != instanceSoid)
        || (source.instanceSoid != 0 && instance_occurrences(current, source.instanceSoid) != 1))
        return false;
    UnlockContext context{};
    context.sourceInstanceSoid = source.instanceSoid;
    context.itemHash = item.definitionHash;
    context.itemIndex = index;
    context.bucketSelector = selector;
    context.requestHasInstance = hasInstance;
    reason = "unlock_ownership_mapping";
    // Acquisition already sets this flag (native 520E13 -> 555EC0). Consuming the held
    // Unlock copy retains it; an older delivery can acquire the same mapped flag here.
    if (!prepare_native_ownership(index, true, context.ownership)) return false;
    std::unique_ptr<AccountState> working(new (std::nothrow) AccountState(current));
    reason = "unlock_after_image";
    if (!working) return false;
    for (std::size_t i = sourceIndex; i + 1 < working->profileItemCount; ++i)
        working->profileItems[i] = working->profileItems[i + 1];
    working->profileItems[--working->profileItemCount] = {};
    loadout::ResolvedLoadout resolved{};
    if (!account::valid(*working) || !helpers::valid_profile_inventory(*working)
        || !loadout::resolve(*working, characterIndex, resolved)
        || !helpers::character_encoding_preflight(*working, characterIndex, resolved, false))
        return false;
    mutation.beforeCharacter = mutation.afterCharacter = current.characters[characterIndex];
    mutation.beforeProfileItems = current.profileItems;
    mutation.afterProfileItems = working->profileItems;
    mutation.beforeProfileItemCount = current.profileItemCount;
    mutation.afterProfileItemCount = working->profileItemCount;
    mutation.accountSoid = current.primarySoid;
    mutation.characterSoid = current.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.cosmeticUnlock = context;
    mutation.prepared = true;
    reason = "prepared";
    return true;
}
} // namespace

bool supports_manual_unlock(std::uint16_t itemIndex) noexcept {
    items::Definition item{};
    return unlock_definition(itemIndex, item);
}

bool prepare_unlock(bool hasInstance,
                    std::uint64_t instanceSoid,
                    std::uint16_t itemIndex,
                    std::int32_t expectedQuantity,
                    std::int8_t bucketSelector,
                    PendingRecordRewardGrant& mutation,
                    const char*& reason) noexcept {
    mutation = {};
    reason = "investment_database_unavailable";
    std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!current || !transaction.ready() || !store::read_account(*current)
        || !stage_unlock(*current, hasInstance, instanceSoid, itemIndex, expectedQuantity,
                         bucketSelector, mutation, reason)) {
        mutation = {};
        return false;
    }
    if (!transaction.commit()) {
        reason = "investment_read_transaction_failed";
        mutation = {};
        return false;
    }
    return true;
}

bool materialize_unlock(const AccountState& current,
                        const PendingRecordRewardGrant& mutation,
                        AccountState& after) noexcept {
    if (!mutation.prepared || !mutation.cosmeticUnlock || mutation.everversePackage || mutation.brightEngramRedemption
        || mutation.everversePurchase || mutation.pursuitRedemption || mutation.dawningDelivery
        || mutation.beforeDawning || mutation.afterDawning || mutation.rewardCount != 0
        || mutation.claimedRecordIndex != kUnclaimedRecordIndex
        || mutation.accountSoid != current.primarySoid
        || mutation.characterIndex >= current.characterCount
        || !helpers::same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !helpers::same_profile_inventory(current, mutation.beforeProfileItems, mutation.beforeProfileItemCount))
        return false;
    for (const auto& reward : mutation.rewards)
        if (reward.instanceSoid || reward.definitionHash || reward.stateIndex || reward.quantity
            || reward.afterQuantity || reward.mutationSerial || reward.inventoryRow
            || reward.kind != RecordRewardKind::characterInstance || reward.appendedProfileResident)
            return false;
    std::unique_ptr<PendingRecordRewardGrant> canonical(new (std::nothrow) PendingRecordRewardGrant);
    store::Transaction transaction;
    const auto& context = *mutation.cosmeticUnlock;
    const char* reason{};
    if (!canonical || !transaction.ready()
        || !stage_unlock(current, context.requestHasInstance,
                         context.requestHasInstance ? context.sourceInstanceSoid : 0,
                         context.itemIndex, 1, context.bucketSelector, *canonical, reason)
        || canonical->cosmeticUnlock != mutation.cosmeticUnlock
        || canonical->accountSoid != mutation.accountSoid
        || canonical->characterSoid != mutation.characterSoid
        || canonical->characterIndex != mutation.characterIndex
        || !helpers::same_character(canonical->afterCharacter, mutation.afterCharacter)
        || !helpers::same_profile_views(canonical->afterProfileItems, canonical->afterProfileItemCount,
                                        mutation.afterProfileItems, mutation.afterProfileItemCount))
        return false;
    after = current;
    after.profileItems = canonical->afterProfileItems;
    after.profileItemCount = canonical->afterProfileItemCount;
    return transaction.commit();
}

bool commit_unlock(const PendingRecordRewardGrant& mutation) noexcept {
    std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    std::unique_ptr<AccountState> after(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!current || !after || !transaction.ready() || !store::read_account(*current)
        || !materialize_unlock(*current, mutation, *after))
        return false;
    const auto& context = *mutation.cosmeticUnlock;
    return store::write_account(*after) && write_native_ownership(context.ownership)
           && record_ownership(current->primarySoid, context.itemHash) && transaction.commit();
}

} // namespace sunrise::state::eververse
