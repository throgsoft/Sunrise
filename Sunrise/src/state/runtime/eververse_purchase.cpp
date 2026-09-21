#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>

#include "../../core/runtime/wall_clock.h"
#include "../build_data/eververse/manifest_catalog.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "../investment/store_internal.h"
#include "eververse_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::eververse {
namespace {
namespace data = build_data;
namespace vendors = data::vendors;
namespace helpers = runtime::detail;
namespace store = investment::store;

bool read_purchases(std::uint64_t accountSoid,
                    const PurchaseContext& context,
                    std::int32_t& count) noexcept {
    count = 0;
    store::Statement row("SELECT purchases FROM eververse_purchases WHERE account_soid=? "
                         "AND vendor_hash=? AND sale_index=? AND item_hash=?");
    if (!row.parameters(accountSoid, context.vendorHash, context.saleIndex, context.itemHash))
        return false;
    const int result = row.step();
    if (result == SQLITE_DONE) return true;
    return result == SQLITE_ROW && row.column(0, count) && count > 0 && row.step() == SQLITE_DONE;
}

bool record_purchase(std::uint64_t accountSoid, const PurchaseContext& context) noexcept {
    if (context.purchasesBefore == (std::numeric_limits<std::int32_t>::max)()) return false;
    std::int32_t current{};
    if (!read_purchases(accountSoid, context, current) || current != context.purchasesBefore)
        return false;
    store::Statement row(
        "INSERT INTO eververse_purchases "
        "(account_soid,vendor_hash,sale_index,item_hash,purchases) VALUES (?,?,?,?,?) "
        "ON CONFLICT(account_soid,vendor_hash,sale_index,item_hash) "
        "DO UPDATE SET purchases=excluded.purchases");
    return row.write(
        accountSoid, context.vendorHash, context.saleIndex, context.itemHash, current + 1);
}

bool holds_item(const AccountState& account, std::uint32_t hash) noexcept {
    for (std::size_t i = 0; i < account.profileItemCount; ++i)
        if (account.profileItems[i].definitionHash == hash
            || account.profileItems[i].wrappedItemHash == hash) return true;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        for (const auto& equipped : character.equipment.slots)
            if (equipped && equipped->definitionHash == hash) return true;
        for (std::size_t i = 0; i < character.inventory.count; ++i)
            if (character.inventory.values[i].definitionHash == hash) return true;
        for (std::size_t i = 0; i < character.stacks.count; ++i)
            if (character.stacks.values[i].definitionHash == hash) return true;
    }
    return false;
}

bool purchase_wrapper(std::uint16_t index, std::uint8_t primaryBucket) noexcept {
    data::items::Definition item{};
    data::items::details::Definition detail{};
    data::eververse::ItemMetadata metadata{};
    return data::find_item_definition_index(index, item) && item.definitionIndex == index
           && item.bucketId == primaryBucket
           && data::find_configured_item_detail(index, detail)
           && detail.definitionIndex == index && detail.definitionHash == item.definitionHash
           && detail.bucketId == item.bucketId && detail.maxStackSize == 1
           && detail.instancedDefinitionState == data::items::details::InstancedDefinitionState::instanced
           && data::eververse::read_item(item.definitionHash, metadata)
           && metadata.itemHash == item.definitionHash && metadata.itemIndex == index
           && metadata.bucketIndex == item.bucketId && metadata.maxStackSize == 1
           && metadata.isWrapper && metadata.instanced && !metadata.isDummy
           && metadata.previewVendorHash == 0 && !metadata.useOnAcquire
           && !metadata.onActionRecreateSelf && !metadata.unlockAction;
}

bool installed_metadata(const data::eververse::ItemMetadata& metadata,
                        data::items::Definition& item,
                        data::items::details::Definition& detail) noexcept {
    return data::find_item_definition_index(metadata.itemIndex, item)
           && item.definitionIndex == metadata.itemIndex && item.definitionHash == metadata.itemHash
           && item.bucketId == metadata.bucketIndex
           && data::find_configured_item_detail(metadata.itemIndex, detail)
           && detail.definitionIndex == item.definitionIndex && detail.definitionHash == item.definitionHash
           && detail.bucketId == item.bucketId && detail.maxStackSize == metadata.maxStackSize
           && metadata.instanced == (detail.instancedDefinitionState
                                      == data::items::details::InstancedDefinitionState::instanced);
}

bool starter_pack_rewards(PurchaseContext& context,
                          std::array<DirectRecordReward, kRecordRewardGrantCapacity>& rewards,
                          std::size_t& count,
                          const char*& reason) noexcept {
    data::eververse::StarterPack pack{};
    vendors::IndexEntry entry{};
    vendors::Definition preview{};
    reason = "starter_pack_preview_mismatch";
    if (context.itemHash != data::eververse::kArrivalsStarterPackHash
        || !data::eververse::read_starter_pack(pack)
        || !vendors::find_index(pack.previewVendorIndex, entry)
        || entry.definitionHash != context.itemHash || !vendors::find(entry.definitionHash, preview)
        || preview.index != entry.index || preview.definitionTag != entry.definitionTag
        || preview.saleCount != pack.entries.size() || pack.entries.size() > rewards.size())
        return false;
    std::size_t currencyCount{}, instanceCount{}, consumableCount{};
    for (std::size_t i = 0; i < pack.entries.size(); ++i) {
        const auto& advertised = pack.entries[i].advertised;
        const auto quantity = pack.entries[i].quantity;
        vendors::SaleRow native{};
        data::items::Definition item{};
        data::items::details::Definition detail{};
        data::inventory::buckets::Descriptor bucket{};
        if (!vendors::sale_row(preview, i, native) || native.itemIndex != advertised.itemIndex
            || native.quantity != quantity || native.secondaryItemIndex != vendors::kAbsentSecondaryItem
            || native.costCount != 0 || native.costQuantity != 0
            || native.costItemIndex != vendors::kAbsentCostItem
            || !installed_metadata(advertised, item, detail)
            || !data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.bucketId != item.bucketId)
            return false;
        reason = "starter_pack_unsupported_content";
        if (advertised.isWrapper || advertised.previewVendorHash != 0 || advertised.useOnAcquire
            || advertised.onActionRecreateSelf || advertised.unlockAction
            || detail.objectiveCount != 0 || detail.rewardCount != 0)
            return false;
        if (advertised.itemHash == 1065442101U) {
            // Source-bound Sunrise interpretation of this guaranteed preview marker only.
            // No general native alias or bounty cadence amount is inferred.
            data::eververse::ItemMetadata wallet{};
            data::items::Definition paid{};
            data::items::details::Definition paidDetail{};
            if (advertised.itemIndex != 1255 || item.bucketId != 37 || !advertised.isDummy
                || advertised.instanced || detail.maxStackSize != 1 || quantity != 250
                || detail.acquiredFlagSlot != 0xFFFFU
                || !data::eververse::read_item(kBrightDustHash, wallet)
                || !installed_metadata(wallet, paid, paidDetail)
                || paid.definitionIndex != 129 || paid.bucketId != 24
                || wallet.isDummy || wallet.instanced || wallet.useOnAcquire || wallet.isWrapper
                || wallet.previewVendorHash != 0 || paidDetail.equipmentSlot
                || paidDetail.acquiredFlagSlot != 0xFFFFU)
                return false;
            rewards[i] = {paid.definitionIndex, quantity};
            ++currencyCount;
            continue;
        }
        if (advertised.isDummy || quantity <= 0 || quantity > detail.maxStackSize) return false;
        if (advertised.instanced) {
            if (quantity != 1 || !detail.equipmentSlot
                || bucket.arraySelector != data::inventory::buckets::ArraySelector::character
                || (item.bucketId != 9 && item.bucketId != 10)
                || context.extraOwnershipCount == context.extraOwnership.size()
                || !prepare_native_ownership(item.definitionIndex, true,
                                              context.extraOwnership[context.extraOwnershipCount]))
                return false;
            ++context.extraOwnershipCount;
            ++instanceCount;
        } else {
            if (item.bucketId != 15 || detail.equipmentSlot || detail.acquiredFlagSlot != 0xFFFFU
                || bucket.arraySelector != data::inventory::buckets::ArraySelector::profile)
                return false;
            ++consumableCount;
        }
        rewards[i] = {item.definitionIndex, quantity};
    }
    if (currencyCount != 1 || instanceCount != 2 || consumableCount != 3
        || !visit_purchase_ownership(context, [](const NativeOwnership&) { return true; }))
        return false;
    count = pack.entries.size();
    return true;
}

bool stage(const AccountState& account,
           std::int32_t vendorIndex,
           std::int32_t saleIndex,
           std::int64_t purchasedAt,
           PendingRecordRewardGrant& mutation,
           const char*& reason) noexcept {
    mutation = {};
    reason = "sale_identity";
    if (!account::valid(account) || !helpers::valid_profile_inventory(account) || vendorIndex < 0
        || vendorIndex > 0xFFFF || saleIndex < 0 || saleIndex > 0xFFFF)
        return false;
    vendors::IndexEntry entry{};
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    data::items::Definition item{}, currency{};
    data::items::details::Definition itemDetail{};
    if (!vendors::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
        || entry.definitionHash != kStoreVendorHash || !vendors::find(entry.definitionHash, vendor)
        || vendor.index != entry.index || vendor.definitionTag != entry.definitionTag
        || !vendors::sale_row(vendor, static_cast<std::size_t>(saleIndex), sale)
        || !data::find_item_definition_index(sale.itemIndex, item)
        || !data::find_configured_item_detail(sale.itemIndex, itemDetail)
        || itemDetail.definitionHash != item.definitionHash || itemDetail.bucketId != item.bucketId)
        return false;
    reason = "unsupported_price_or_currency";
    if (sale.costCount != 1 || !sale.costIsConstant || sale.costQuantity == 0
        || sale.costQuantity
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || !data::find_item_definition_index(sale.costItemIndex, currency)
        || (currency.definitionHash != kSilverHash && currency.definitionHash != kBrightDustHash))
        return false;
    reason = "unsupported_purchase_limit";
    if (sale.purchaseGate != vendors::PurchaseGate::unrestricted
        && sale.purchaseGate != vendors::PurchaseGate::notOwned)
        return false;
    reason = "unsupported_quantity";
    if (sale.quantity <= 0) return false;
    data::eververse::ItemMetadata metadata{};
    reason = "cosmetic_metadata_unavailable";
    if (!data::eververse::read_item(item.definitionHash, metadata)
        || metadata.itemIndex != item.definitionIndex || metadata.bucketIndex != item.bucketId
        || metadata.maxStackSize != itemDetail.maxStackSize
        || metadata.instanced
               != (itemDetail.instancedDefinitionState
                   == data::items::details::InstancedDefinitionState::instanced))
        return false;
    reason = "unsupported_on_acquire_or_display_item";
    const bool starterPack = item.definitionHash == data::eververse::kArrivalsStarterPackHash;
    if (starterPack) {
        reason = "starter_pack_sale_mismatch";
        // Native item 12354 has +0xBB=1, matching the manifest's isInstanceItem=true.
        // The checked six-row expansion below delivers its contents, not the source instance.
        if (item.definitionIndex != 12354 || saleIndex != 47 || sale.quantity != 1
            || currency.definitionHash != kSilverHash || sale.costQuantity != 800
            || sale.purchaseGate != vendors::PurchaseGate::notOwned
            || metadata.previewVendorHash != item.definitionHash || !metadata.instanced)
            return false;
    }
    // Only the source-bound guaranteed starter pack can expand a preview into grants.
    if (metadata.previewVendorHash != 0 || metadata.isWrapper) {
        if (!starterPack || metadata.isWrapper) {
            reason = "bundle_or_refund_wrapper_unresolved";
            return false;
        }
    }
    if (metadata.isDummy || metadata.useOnAcquire || metadata.onActionRecreateSelf
        || (metadata.unlockAction && !metadata.deleteOnAction))
        return false;
    reason = "unsupported_manual_unlock_action";
    if (metadata.unlockAction
        && (sale.quantity != 1 || !supports_manual_unlock(item.definitionIndex)))
        return false;
    reason = "unlock_delivery_already_held";
    if (metadata.unlockAction && holds_item(account, item.definitionHash)) return false;

    const bool refundable = sale.refundPolicy == vendors::RefundPolicy::refundable;
    const bool wrapped = sale.secondaryItemIndex != vendors::kAbsentSecondaryItem;
    // A refundable sale must retain its paid source until Open or Refund. Instanced contents
    // and self-contained bundles need their own native creation inputs before admission.
    if (refundable || wrapped) {
        reason = "unsupported_refundable_content";
        if (!refundable || !wrapped || currency.definitionHash != kSilverHash
            || sale.quantity != 1 || metadata.instanced || !metadata.unlockAction
            || !purchase_wrapper(sale.secondaryItemIndex, item.bucketId))
            return false;
    }

    PurchaseContext context{};
    context.vendorHash = entry.definitionHash;
    context.itemHash = item.definitionHash;
    context.currencyHash = currency.definitionHash;
    context.cost = sale.costQuantity;
    context.quantity = sale.quantity;
    context.vendorIndex = entry.index;
    context.saleIndex = static_cast<std::uint16_t>(saleIndex);
    context.secondaryWrapperIndex = sale.secondaryItemIndex;
    context.recordOwnership = itemDetail.acquiredFlagSlot != 0xFFFFU;
    reason = "acquisition_mapping_unavailable";
    if (context.recordOwnership) {
        if (!prepare_native_ownership(item.definitionIndex, true, context.ownership))
            return false;
    } else if (metadata.unlockAction || metadata.collectibleHash != 0
               || sale.purchaseGate == vendors::PurchaseGate::notOwned) {
        return false;
    }
    if (sale.purchaseGate == vendors::PurchaseGate::notOwned) {
        reason = "purchase_gate_mismatch";
        if (!context.ownership.prepared || sale.purchaseUnlockSlot != context.ownership.unlockSlot)
            return false;
        bool received{};
        reason = "ownership_state_unavailable";
        if (!read_ownership(account.primarySoid, item.definitionHash, received)) return false;
        reason = "already_owned_or_delivered";
        // Supported direct acquisitions remain owned after the delivered item is discarded.
        if (context.ownership.before == unlocks::kFlagSet || received
            || holds_item(account, item.definitionHash))
            return false;
    }
    reason = "purchase_receipt_unavailable";
    if (!read_purchases(account.primarySoid, context, context.purchasesBefore)
        || context.purchasesBefore == (std::numeric_limits<std::int32_t>::max)())
        return false;
    std::int32_t balance{}, cap{};
    reason = "wallet_unavailable";
    if (!detail::wallet_balance(account, currency.definitionHash, balance, cap)) return false;
    reason = "insufficient_balance";
    if (balance < static_cast<std::int32_t>(sale.costQuantity)) return false;
    std::unique_ptr<AccountState> charged(new (std::nothrow) AccountState(account));
    reason = "wallet_debit_refused";
    if (!charged
        || !detail::stage_wallet_balance(*charged,
                                         currency.definitionHash,
                                         balance - static_cast<std::int32_t>(sale.costQuantity)))
        return false;

    const bool instanced = metadata.instanced;
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rewards{};
    auto count = instanced ? static_cast<std::size_t>(sale.quantity) : std::size_t{1};
    reason = "quantity_exceeds_transaction_capacity";
    if (count > rewards.size()) return false;
    if (starterPack) {
        if (!starter_pack_rewards(context, rewards, count, reason)) return false;
    } else {
        for (std::size_t i = 0; i < count; ++i)
            rewards[i] = {item.definitionIndex, instanced ? 1 : sale.quantity};
    }
    reason = "grant_or_capacity_refused";
    if (wrapped) {
        const auto characterIndex = helpers::selected_character_index(account);
        data::items::Definition wrapper{};
        std::uint64_t minimum{};
        PurchaseReceipt receipt{};
        reason = "receipt_space_or_identity_unavailable";
        if (characterIndex >= account.characterCount
            || charged->profileItemCount >= charged->profileItems.size()
            || !data::find_item_definition_index(sale.secondaryItemIndex, wrapper)
            || !helpers::next_profile_item_instance_soid(account, minimum)
            || !reserve_purchase_identity(account.primarySoid, minimum,
                                           receipt.sourceInstanceSoid, receipt.slot))
            return false;
        // The receipt high-water mark may jump across the first free profile identity onto
        // a still-held cosmetic. Check the resulting candidate against every live domain.
        while (helpers::account_owns_soid(account, receipt.sourceInstanceSoid)) {
            if (receipt.sourceInstanceSoid
                >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
                return false;
            ++receipt.sourceInstanceSoid;
        }
        reason = "receipt_clock_unavailable";
        const auto now = core::runtime::investment_clock_seconds();
        if (purchasedAt <= 0 || purchasedAt > now
            || purchasedAt > (std::numeric_limits<std::int64_t>::max)() - kRefundPeriodSeconds)
            return false;
        receipt.characterSoid = account.characters[characterIndex].soid;
        receipt.wrapperHash = wrapper.definitionHash;
        receipt.itemHash = item.definitionHash;
        receipt.currencyHash = currency.definitionHash;
        receipt.cost = sale.costQuantity;
        receipt.vendorIndex = entry.index;
        receipt.saleIndex = static_cast<std::uint16_t>(saleIndex);
        receipt.purchasedAt = purchasedAt;
        receipt.expiresAt = purchasedAt + kRefundPeriodSeconds;
        std::int32_t serial{};
        for (std::size_t i = 0; i < account.profileItemCount; ++i)
            serial = (std::max)(serial, account.profileItems[i].mutationSerial);
        if (serial == (std::numeric_limits<std::int32_t>::max)()) return false;
        const auto position = charged->profileItemCount++;
        auto& delivery = charged->profileItems[position];
        delivery.instanceSoid = receipt.sourceInstanceSoid;
        delivery.definitionHash = wrapper.definitionHash;
        delivery.quantity = 1;
        delivery.mutationSerial = serial + 1;
        delivery.wrappedItemHash = item.definitionHash;
        reason = "wrapper_bucket_full_or_invalid";
        if (!account::valid(*charged) || !helpers::valid_profile_inventory(*charged)) return false;
        mutation.beforeCharacter = mutation.afterCharacter = account.characters[characterIndex];
        mutation.afterProfileItems = charged->profileItems;
        mutation.afterProfileItemCount = charged->profileItemCount;
        mutation.accountSoid = account.primarySoid;
        mutation.characterSoid = receipt.characterSoid;
        mutation.characterIndex = characterIndex;
        mutation.rewards[0] = {delivery.instanceSoid, delivery.definitionHash, position, 1, 1,
                               delivery.mutationSerial, 0, RecordRewardKind::profileStack, true};
        mutation.rewardCount = 1;
        mutation.prepared = true;
        context.receipt = receipt;
        // An unopened package reserves the sale but does not unlock its cosmetic.
        context.ownership = {};
        context.recordOwnership = false;
    } else if (!helpers::stage_record_reward_grant(
                   *charged, std::span(rewards).first(count), kUnclaimedRecordIndex, mutation)) {
        return false;
    }
    // A removed wallet row can have the largest serial. Never reuse it in grant feedback.
    std::int32_t serial{};
    for (std::size_t i = 0; i < account.profileItemCount; ++i)
        serial = (std::max)(serial, account.profileItems[i].mutationSerial);
    for (std::size_t i = 0; i < charged->profileItemCount; ++i)
        serial = (std::max)(serial, charged->profileItems[i].mutationSerial);
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        auto& reward = mutation.rewards[i];
        if (reward.kind != RecordRewardKind::profileStack) continue;
        if (reward.mutationSerial <= serial) {
            if (serial == (std::numeric_limits<std::int32_t>::max)()) return false;
            reward.mutationSerial = ++serial;
            mutation.afterProfileItems[reward.stateIndex].mutationSerial = reward.mutationSerial;
        } else {
            serial = reward.mutationSerial;
        }
    }
    mutation.beforeProfileItems = account.profileItems;
    mutation.beforeProfileItemCount = account.profileItemCount;
    mutation.everversePurchase = context;
    reason = wrapped ? "prepared_refundable_package" : "prepared";
    return true;
}

bool same_rewards(const PendingRecordRewardGrant& left,
                  const PendingRecordRewardGrant& right) noexcept {
    if (left.rewardCount != right.rewardCount) return false;
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

bool is_store_vendor(std::int32_t vendorIndex) noexcept {
    vendors::IndexEntry entry{};
    return vendorIndex >= 0 && vendorIndex <= 0xFFFF
           && vendors::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
           && entry.definitionHash == kStoreVendorHash;
}

bool prepare_purchase(std::int32_t vendorIndex,
                      std::int32_t saleIndex,
                      PendingRecordRewardGrant& mutation,
                      const char*& reason) noexcept {
    mutation = {};
    reason = "investment_database_unavailable";
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!account || !transaction.ready() || !store::read_account(*account)
        || !stage(*account, vendorIndex, saleIndex, core::runtime::investment_clock_seconds(),
                  mutation, reason)) {
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

bool materialize_purchase(const AccountState& current,
                          const PendingRecordRewardGrant& mutation,
                          AccountState& after) noexcept {
    if (!mutation.prepared || !mutation.everversePurchase || mutation.everversePackage || mutation.cosmeticUnlock || mutation.pursuitRedemption
        || mutation.brightEngramRedemption || mutation.dawningDelivery
        || mutation.beforeDawning || mutation.afterDawning
        || mutation.claimedRecordIndex != kUnclaimedRecordIndex
        || mutation.accountSoid != current.primarySoid
        || mutation.characterIndex >= current.characterCount
        || helpers::selected_character_index(current) != mutation.characterIndex
        || !helpers::same_character(current.characters[mutation.characterIndex],
                                    mutation.beforeCharacter)
        || !helpers::same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.beforeProfileItemCount))
        return false;
    std::unique_ptr<PendingRecordRewardGrant> canonical(new (std::nothrow)
                                                            PendingRecordRewardGrant);
    if (!canonical) return false;
    store::Transaction transaction;
    const auto& context = *mutation.everversePurchase;
    const char* reason{};
    if (!transaction.ready()
        || !stage(current, context.vendorIndex, context.saleIndex,
                  context.receipt ? context.receipt->purchasedAt : core::runtime::investment_clock_seconds(),
                  *canonical, reason)
        || canonical->everversePurchase != mutation.everversePurchase
        || canonical->accountSoid != mutation.accountSoid
        || canonical->characterSoid != mutation.characterSoid
        || canonical->characterIndex != mutation.characterIndex
        || !helpers::same_character(canonical->afterCharacter, mutation.afterCharacter)
        || !helpers::same_profile_views(canonical->afterProfileItems,
                                        canonical->afterProfileItemCount,
                                        mutation.afterProfileItems,
                                        mutation.afterProfileItemCount)
        || !same_rewards(*canonical, mutation))
        return false;
    after = current;
    after.characters[mutation.characterIndex] = canonical->afterCharacter;
    after.profileItems = canonical->afterProfileItems;
    after.profileItemCount = canonical->afterProfileItemCount;
    return account::valid(after) && helpers::valid_profile_inventory(after) && transaction.commit();
}

bool commit_purchase(const PendingRecordRewardGrant& mutation) noexcept {
    std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    std::unique_ptr<AccountState> after(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!current || !after || !transaction.ready() || !store::read_account(*current)
        || !materialize_purchase(*current, mutation, *after))
        return false;
    const auto& context = *mutation.everversePurchase;
    return store::write_account(*after)
           && visit_purchase_ownership(context, [&](const NativeOwnership& ownership) {
                  data::items::Definition item{};
                  return data::find_item_definition_index(ownership.itemIndex, item)
                         && write_native_ownership(ownership)
                         && record_ownership(current->primarySoid, item.definitionHash);
              })
           && record_purchase(current->primarySoid, context)
           && (!context.receipt || insert_purchase_receipt(current->primarySoid, *context.receipt))
           && transaction.commit();
}

} // namespace sunrise::state::eververse
