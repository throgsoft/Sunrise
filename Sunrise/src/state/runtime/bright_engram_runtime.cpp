#include "bright_engram_runtime.h"

#include <array>
#include <algorithm>
#include <memory>
#include <new>

#include "../../core/logging/log.h"
#include "../investment/store_internal.h"
#include "character_encoding_preflight.h"
#include "eververse_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::bright_engrams {
namespace {
using namespace runtime::detail;
namespace catalog = build_data::eververse;
namespace inventory = account::inventory;
namespace buckets = build_data::inventory::buckets;
namespace items = build_data::items;
namespace loadout = middleware::datagen::family4::loadout;

void report(const char* stage, const char* reason) noexcept {
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=bright_engram stage=%s result=refused reason=%s",
                      stage,
                      reason);
}

/** A manifest can describe the installed build only when its native identity/routing agrees. */
[[nodiscard]] bool installed(const catalog::ItemMetadata& metadata) noexcept {
    items::Definition item{};
    items::details::Definition detail{};
    buckets::Descriptor bucket{};
    return build_data::find_item_definition_hash(metadata.itemHash, item)
           && item.definitionIndex == metadata.itemIndex && item.bucketId == metadata.bucketIndex
           && build_data::find_configured_item_detail(item.definitionIndex, detail)
           && detail.definitionHash == item.definitionHash
           && detail.definitionIndex == item.definitionIndex && detail.bucketId == item.bucketId
           && detail.maxStackSize == metadata.maxStackSize
           && (detail.instancedDefinitionState
               == items::details::InstancedDefinitionState::instanced)
                  == metadata.instanced
           && build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
           && (bucket.arraySelector == buckets::ArraySelector::character
               || bucket.arraySelector == buckets::ArraySelector::profile);
}

/** A held bright source uses the ordinary character Engrams bucket and no acquire effect. */
[[nodiscard]] bool installed_held_source(const catalog::ItemMetadata& metadata) noexcept {
    items::details::Definition detail{};
    return metadata.bucketIndex == 31 && metadata.instanced && metadata.maxStackSize == 1
           && !metadata.isDummy && !metadata.isWrapper && !metadata.unlockAction
           && !metadata.useOnAcquire && !metadata.onActionRecreateSelf
           && installed(metadata)
           && build_data::find_configured_item_detail(metadata.itemIndex, detail)
           && !detail.equipmentSlot && detail.objectiveCount == 0 && detail.lifetimeSeconds == 0
           && detail.acquireEffectIndex && *detail.acquireEffectIndex == 0xFFFFU;
}

/** A delivered Unlock copy is already acquired, even before its explicit Unlock action. */
[[nodiscard]] bool holds_reward(const AccountState& account, std::uint32_t hash) noexcept {
    for (std::size_t i = 0; i < account.profileItemCount; ++i)
        if (account.profileItems[i].definitionHash == hash && account.profileItems[i].quantity > 0)
            return true;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        for (const auto& equipped : character.equipment.slots)
            if (equipped && equipped->definitionHash == hash) return true;
        // Includes Lost Items: an unclaimed Postmaster delivery still belongs to this account.
        for (std::size_t i = 0; i < character.inventory.count; ++i)
            if (character.inventory.values[i].definitionHash == hash) return true;
        for (std::size_t i = 0; i < character.stacks.count; ++i)
            if (character.stacks.values[i].definitionHash == hash && character.stacks.values[i].quantity > 0)
                return true;
    }
    return false;
}

[[nodiscard]] bool eligible_rewards(const AccountState& account,
                                    const catalog::EngramCatalog& definitions,
                                    std::vector<std::uint32_t>& hashes) noexcept {
    hashes.clear();
    try {
        for (const auto& entry : definitions.entries) {
            if (!catalog::selectable_engram_entry(entry)) continue;
            const auto& item = entry.candidates.front();
            if (!installed(item)) continue;
            if (item.unlockAction) {
                if (!eververse::supports_manual_unlock(item.itemIndex)) continue;
                eververse::NativeOwnership ownership{};
                bool received{};
                if (!eververse::prepare_native_ownership(item.itemIndex, false, ownership)
                    || !eververse::read_ownership(account.primarySoid, item.itemHash, received))
                    return false;
                if (received || ownership.before == unlocks::kFlagSet
                    || holds_reward(account, item.itemHash)) continue;
            }
            hashes.push_back(item.itemHash);
        }
    } catch (...) {
        hashes.clear();
        return false;
    }
    return true;
}

/** Count across every ownership domain, rejecting duplicate, equipped, and other-character IDs. */
[[nodiscard]] bool source_location(const AccountState& account,
                                   std::uint64_t soid,
                                   std::size_t& characterIndex,
                                   std::size_t& itemIndex) noexcept {
    if (!soid || !account::valid(account) || !valid_profile_inventory(account)) return false;
    characterIndex = account.characterCount;
    std::size_t selectedCount = 0, occurrences = 0;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        selectedCount += character.selected;
        for (const auto& equipped : character.equipment.slots)
            occurrences += equipped && equipped->instanceSoid == soid;
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            if (character.inventory.values[i].instanceSoid != soid) continue;
            ++occurrences;
            if (character.selected) {
                characterIndex = c;
                itemIndex = i;
            }
        }
    }
    for (std::size_t i = 0; i < account.profileItemCount; ++i)
        occurrences += account.profileItems[i].instanceSoid == soid;
    return selectedCount == 1 && occurrences == 1 && characterIndex < account.characterCount;
}

[[nodiscard]] bool stage(const AccountState& current,
                         std::uint64_t sourceSoid,
                         std::int16_t requestedIndex,
                         PendingRecordRewardGrant& mutation) noexcept {
    mutation = {};
    std::size_t characterIndex = 0, sourceIndex = 0;
    if (!source_location(current, sourceSoid, characterIndex, sourceIndex)) return false;
    const auto& before = current.characters[characterIndex];
    const auto& source = before.inventory.values[sourceIndex];
    if (source.quantity != 1 || source.placement != inventory::ItemPlacement::inventory
        || (source.flags & inventory::kLockedItemFlag) != 0) return false;
    catalog::EngramCatalog definitions{};
    if (!catalog::read_engram_catalog(source.definitionHash, definitions)) {
        report("catalog", "missing_or_unsupported_manifest");
        return false;
    }
    catalog::EngramSelection selection{};
    std::vector<std::uint32_t> eligible{};
    if (!installed_held_source(definitions.source) || requestedIndex < -1
        || (requestedIndex >= 0 && requestedIndex != definitions.source.itemIndex)
        || !eligible_rewards(current, definitions, eligible)
        || !catalog::select_engram_reward(definitions, current.primarySoid, sourceSoid, eligible, selection)
        || !installed(selection.item)) {
        report("catalog", "installed_identity_or_route");
        return false;
    }
    eververse::NativeOwnership ownership{};
    // Native acquisition marks the account flag on delivery; Unlock later removes the copy.
    // Match the established Store route so the flag, receipt, and item publish atomically.
    if (!eververse::prepare_native_ownership(
            selection.item.itemIndex, true, ownership)) {
        report("ownership", "missing_native_mapping");
        return false;
    }
    buckets::Descriptor sourceBucket{};
    loadout::ResolvedLoadout beforeLoadout{};
    std::uint16_t sourceRow = 0;
    std::uint8_t sourceSlot = 0;
    if (!build_data::find_inventory_bucket_descriptor(definitions.source.bucketIndex, sourceBucket)
        || sourceBucket.arraySelector != buckets::ArraySelector::character
        || !loadout::resolve(current, characterIndex, beforeLoadout)
        || !find_unequipped_row(beforeLoadout, sourceSoid, sourceRow, sourceSlot))
        return false;

    const std::unique_ptr<AccountState> working(new (std::nothrow) AccountState(current));
    if (!working) return false;
    auto& character = working->characters[characterIndex];
    for (std::size_t i = sourceIndex; i + 1 < character.inventory.count; ++i)
        character.inventory.values[i] = character.inventory.values[i + 1];
    character.inventory.values[--character.inventory.count] = {};
    const std::array<DirectRecordReward, 1> rewards{
        {{selection.item.itemIndex, selection.quantity}}};
    // This path refuses partial stack credits and bucket overflow. The removed source slot is
    // already available, and no Postmaster or saturation policy can consume an unpaid source.
    if (!stage_record_reward_grant(*working, rewards, kUnclaimedRecordIndex, mutation)
        || mutation.rewardCount != 1 || mutation.rewards[0].quantity != selection.quantity) {
        report("grant", "capacity_or_unsupported_route");
        mutation = {};
        return false;
    }
    auto& reward = mutation.rewards[0];
    // The removed source may be the greatest generated key. Do not reuse its resident identity
    // for the reward: the Queuez transaction must release that exact original object.
    if (reward.instanceSoid == sourceSoid) {
        std::uint64_t replacement = 0;
        if (reward.kind == RecordRewardKind::characterInstance) {
            if (!next_item_instance_soid(current, replacement)) return false;
            mutation.afterCharacter.inventory.values[reward.stateIndex].instanceSoid = replacement;
        } else if (reward.kind == RecordRewardKind::profileStack) {
            if (!next_profile_item_instance_soid(current, replacement)) return false;
            mutation.afterProfileItems[reward.stateIndex].instanceSoid = replacement;
        } else
            return false;
        reward.instanceSoid = replacement;
    }
    working->characters[characterIndex] = mutation.afterCharacter;
    working->profileItems = mutation.afterProfileItems;
    working->profileItemCount = mutation.afterProfileItemCount;
    loadout::ResolvedLoadout afterLoadout{};
    if (!account::valid(*working) || !valid_profile_inventory(*working)
        || !loadout::resolve(*working, characterIndex, afterLoadout)
        || loadout_contains(afterLoadout, sourceSoid)
        || !character_encoding_preflight(*working, characterIndex, afterLoadout))
        return false;
    if (reward.kind == RecordRewardKind::characterInstance) {
        std::uint8_t slot = 0;
        if (!find_unequipped_row(afterLoadout, reward.instanceSoid, reward.inventoryRow, slot))
            return false;
    }
    mutation.beforeCharacter = before;
    mutation.beforeProfileItems = current.profileItems;
    mutation.beforeProfileItemCount = current.profileItemCount;
    mutation.brightEngramRedemption =
        RedemptionContext{sourceSoid, source.definitionHash, selection, ownership};
    return true;
}

[[nodiscard]] bool same_rewards(const PendingRecordRewardGrant& left,
                                const PendingRecordRewardGrant& right) noexcept {
    for (std::size_t i = 0; i < left.rewards.size(); ++i) {
        const auto& a = left.rewards[i];
        const auto& b = right.rewards[i];
        if (a.instanceSoid != b.instanceSoid || a.definitionHash != b.definitionHash
            || a.stateIndex != b.stateIndex || a.quantity != b.quantity
            || a.afterQuantity != b.afterQuantity || a.mutationSerial != b.mutationSerial
            || a.inventoryRow != b.inventoryRow || a.kind != b.kind
            || a.appendedProfileResident != b.appendedProfileResident)
            return false;
    }
    return true;
}
} // namespace

GrantSourceKind classify_grant_source(std::uint16_t itemIndex,
                                      std::uint32_t expectedHash) noexcept {
    items::Definition item{};
    if (!expectedHash || !build_data::find_item_definition_index(itemIndex, item)
        || item.definitionHash != expectedHash) return GrantSourceKind::unsupportedVariant;
    const auto ingredient = inventory::dawning::ingredient(expectedHash);
    if (ingredient < inventory::dawning::kIngredientCount
        && expectedHash == inventory::dawning::kIngredients[ingredient].pickupHash)
        return GrantSourceKind::unrelated;
    // Other inventory classes retain their existing concrete grant policy.
    if (item.bucketId != 31 && item.bucketId != 37) return GrantSourceKind::unrelated;
    catalog::ItemMetadata metadata{};
    if (!catalog::read_item(expectedHash, metadata)) return GrantSourceKind::unrelated;
    if (metadata.itemIndex != itemIndex || !installed(metadata))
        return GrantSourceKind::unsupportedVariant;
    if (!metadata.previewVendorHash) return GrantSourceKind::unrelated;
    if (metadata.isDummy) return GrantSourceKind::preview;

    catalog::EngramCatalog definitions{};
    // read_engram_catalog requires sack.vendorSackType=engram.silver, openOnAcquire=false,
    // a complete inhibited preview hierarchy, and concrete local reward metadata.
    if (installed_held_source(metadata)
        && catalog::read_engram_catalog(expectedHash, definitions)
        && definitions.source == metadata
        && std::any_of(definitions.entries.begin(), definitions.entries.end(),
                       catalog::selectable_engram_entry)) return GrantSourceKind::held;
    // Some auto-open variants point at the same preview as the actual held source. Recognize
    // that relationship from installed data without treating a shared display name as identity.
    if (catalog::read_engram_catalog(metadata.previewVendorHash, definitions)
        && definitions.source.previewVendorHash == metadata.previewVendorHash
        && installed_held_source(definitions.source)) return GrantSourceKind::unsupportedVariant;
    return GrantSourceKind::unrelated;
}

bool prepare_redemption(std::uint64_t sourceSoid,
                        std::int16_t requestedDefinitionIndex,
                        PendingRecordRewardGrant& mutation) noexcept {
    investment::store::Transaction transaction;
    const std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    if (transaction.ready() && current && investment::store::read_account(*current)
        && stage(*current, sourceSoid, requestedDefinitionIndex, mutation) && transaction.commit())
        return true;
    mutation = {};
    return false;
}

bool materialize_redemption(const AccountState& current,
                            const PendingRecordRewardGrant& mutation,
                            AccountState& after) noexcept {
    after = {};
    investment::store::Transaction transaction;
    if (!transaction.ready() || !mutation.prepared || !mutation.brightEngramRedemption || mutation.everversePackage || mutation.everversePurchase
        || mutation.pursuitRedemption
        || mutation.dawningDelivery || mutation.beforeDawning || mutation.afterDawning
        || mutation.claimedRecordIndex != kUnclaimedRecordIndex || !mutation.accountSoid
        || current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.beforeProfileItemCount))
        return false;
    const std::unique_ptr<PendingRecordRewardGrant> canonical(new (std::nothrow)
                                                                  PendingRecordRewardGrant);
    if (!canonical
        || !stage(current, mutation.brightEngramRedemption->sourceInstanceSoid, -1, *canonical)
        || canonical->brightEngramRedemption != mutation.brightEngramRedemption
        || canonical->accountSoid != mutation.accountSoid
        || canonical->characterSoid != mutation.characterSoid
        || canonical->characterIndex != mutation.characterIndex
        || canonical->rewardCount != mutation.rewardCount
        || !same_character(canonical->afterCharacter, mutation.afterCharacter)
        || !same_profile_views(canonical->afterProfileItems,
                               canonical->afterProfileItemCount,
                               mutation.afterProfileItems,
                               mutation.afterProfileItemCount)
        || !same_rewards(*canonical, mutation))
        return false;
    after = current;
    after.characters[canonical->characterIndex] = canonical->afterCharacter;
    after.profileItems = canonical->afterProfileItems;
    after.profileItemCount = canonical->afterProfileItemCount;
    return transaction.commit();
}

bool commit_redemption(PendingRecordRewardGrant& mutation) noexcept {
    investment::store::Transaction transaction;
    const std::unique_ptr<AccountState> after(new (std::nothrow) AccountState);
    if (!transaction.ready() || !after
        || !materialize_redemption(investment::store::account(), mutation, *after))
        return false;
    // Receipt, mapped native flag and physical acquisition share one account transaction.
    // Explicit Unlock consumes the delivered copy without revoking this acquisition.
    return eververse::write_native_ownership(mutation.brightEngramRedemption->ownership)
           && eververse::record_ownership(mutation.accountSoid,
                                          mutation.brightEngramRedemption->selection.item.itemHash)
           && investment::store::write_account(*after) && transaction.commit();
}
} // namespace sunrise::state::bright_engrams
