#include "eververse_internal.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../../core/runtime/wall_clock.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::eververse {
namespace {
namespace helpers = runtime::detail;
namespace store = investment::store;

bool stage_action(const AccountState& current, PackageAction action,
                   std::uint64_t instanceSoid, std::uint16_t itemIndex,
                   std::int32_t expectedQuantity, std::int8_t selector,
                   bool hasClock, std::uint64_t clock,
                   PendingRecordRewardGrant& mutation, const char*& reason) noexcept {
    mutation = {};
    reason = "package_account_or_identity";
    const auto characterIndex = helpers::selected_character_index(current);
    if (!account::valid(current) || !helpers::valid_profile_inventory(current)
        || characterIndex >= current.characterCount || instanceSoid == 0 || expectedQuantity != 1
        || (!hasClock && clock != 0)
        || (action != PackageAction::open && action != PackageAction::refund))
        return false;
    std::array<PurchaseReceipt, kRefundReceiptCapacity> receipts{};
    reason = "receipt_unavailable";
    if (!read_purchase_receipts(current.primarySoid, receipts)) return false;
    const auto found = std::find_if(receipts.begin(), receipts.end(), [&](const auto& receipt) {
        return receipt.sourceInstanceSoid == instanceSoid;
    });
    reason = "package_not_paid_or_already_consumed";
    if (found == receipts.end()) return false;
    const auto& receipt = *found;
    build_data::items::Definition wrapper{}, primary{}, currency{};
    reason = "package_definition_mismatch";
    if (!build_data::find_item_definition_hash(receipt.wrapperHash, wrapper)
        || wrapper.definitionIndex != itemIndex
        || !build_data::find_item_definition_hash(receipt.itemHash, primary)
        || !supports_manual_unlock(primary.definitionIndex)
        || !build_data::find_item_definition_hash(receipt.currencyHash, currency)
        || receipt.currencyHash != kSilverHash
        || (selector != -1 && (selector < 0 || selector != wrapper.bucketId)))
        return false;
    std::size_t sourceIndex = current.profileItemCount, matches{};
    for (std::size_t i = 0; i < current.profileItemCount; ++i) {
        if (current.profileItems[i].instanceSoid != instanceSoid) continue;
        sourceIndex = i;
        ++matches;
    }
    reason = "package_source_missing_or_changed";
    if (matches != 1) return false;
    const auto& source = current.profileItems[sourceIndex];
    if (source.definitionHash != receipt.wrapperHash || source.wrappedItemHash != receipt.itemHash
        || source.quantity != 1)
        return false;
    PackageActionContext context{};
    context.receipt = receipt;
    context.action = action;
    context.bucketSelector = selector;
    context.hasClock = hasClock;
    context.clock = clock;
    const auto now = core::runtime::investment_clock_seconds();
    reason = "receipt_clock_unavailable";
    if (now <= 0 || now < receipt.purchasedAt) return false;
    if (action == PackageAction::refund) {
        reason = "refund_period_expired";
        // The request's timestamp is not the authority for eligibility or the returned price.
        if (now >= receipt.expiresAt) return false;
        std::int32_t balance{}, cap{};
        reason = "refund_wallet_capacity";
        if (!detail::wallet_balance(current, receipt.currencyHash, balance, cap)
            || static_cast<std::int64_t>(balance) + receipt.cost > cap)
            return false;
    } else {
        reason = "package_unlock_mapping_unavailable";
        if (!prepare_native_ownership(primary.definitionIndex, true, context.ownership)) return false;
    }
    std::unique_ptr<AccountState> working(new (std::nothrow) AccountState(current));
    reason = "package_staging_unavailable";
    if (!working) return false;
    for (std::size_t i = sourceIndex; i + 1 < working->profileItemCount; ++i)
        working->profileItems[i] = working->profileItems[i + 1];
    working->profileItems[--working->profileItemCount] = {};
    const std::array<DirectRecordReward, 1> rewards{{
        {action == PackageAction::refund ? currency.definitionIndex : primary.definitionIndex,
         action == PackageAction::refund ? static_cast<std::int32_t>(receipt.cost) : 1}}};
    reason = "package_contents_or_refund_capacity";
    if (!helpers::stage_record_reward_grant(*working, rewards, kUnclaimedRecordIndex, mutation)
        || mutation.rewardCount != 1 || mutation.rewards[0].quantity != rewards[0].quantity)
        return false;
    auto& reward = mutation.rewards[0];
    if (reward.kind != RecordRewardKind::profileStack) return false;
    // Source retirement and the replacement item must be separate QueueZ identities.
    if (reward.instanceSoid == instanceSoid) {
        std::uint64_t replacement{};
        if (!helpers::next_profile_item_instance_soid(current, replacement)) return false;
        mutation.afterProfileItems[reward.stateIndex].instanceSoid = replacement;
        reward.instanceSoid = replacement;
    }
    std::int32_t priorSerial{};
    for (std::size_t i = 0; i < current.profileItemCount; ++i)
        priorSerial = (std::max)(priorSerial, current.profileItems[i].mutationSerial);
    if (reward.mutationSerial <= priorSerial) {
        if (priorSerial == (std::numeric_limits<std::int32_t>::max)()) return false;
        reward.mutationSerial = priorSerial + 1;
        mutation.afterProfileItems[reward.stateIndex].mutationSerial = reward.mutationSerial;
    }
    mutation.beforeProfileItems = current.profileItems;
    mutation.beforeProfileItemCount = current.profileItemCount;
    mutation.everversePackage = context;
    reason = action == PackageAction::refund ? "prepared_refund" : "prepared_open";
    return true;
}

bool same_rewards(const PendingRecordRewardGrant& left, const PendingRecordRewardGrant& right) noexcept {
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

bool prepare_package_action(bool refund, std::uint64_t instanceSoid,
                             std::uint16_t itemIndex, std::int32_t expectedQuantity,
                             std::int8_t bucketSelector, bool hasClock, std::uint64_t clock,
                             PendingRecordRewardGrant& mutation, const char*& reason) noexcept {
    mutation = {};
    reason = "investment_database_unavailable";
    std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!current || !transaction.ready() || !store::read_account(*current)
        || !stage_action(*current, refund ? PackageAction::refund : PackageAction::open,
                         instanceSoid, itemIndex, expectedQuantity, bucketSelector,
                         hasClock, clock, mutation, reason) || !transaction.commit()) {
        mutation = {};
        return false;
    }
    return true;
}

bool materialize_package_action(const AccountState& current,
                                 const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    if (!mutation.prepared || !mutation.everversePackage || mutation.everversePurchase
        || mutation.cosmeticUnlock || mutation.brightEngramRedemption || mutation.pursuitRedemption
        || mutation.dawningDelivery || mutation.beforeDawning || mutation.afterDawning
        || mutation.claimedRecordIndex != kUnclaimedRecordIndex
        || mutation.accountSoid != current.primarySoid
        || mutation.characterIndex >= current.characterCount
        || helpers::selected_character_index(current) != mutation.characterIndex
        || !helpers::same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !helpers::same_profile_inventory(current, mutation.beforeProfileItems, mutation.beforeProfileItemCount))
        return false;
    std::unique_ptr<PendingRecordRewardGrant> canonical(new (std::nothrow) PendingRecordRewardGrant);
    store::Transaction transaction;
    const auto& context = *mutation.everversePackage;
    build_data::items::Definition wrapper{};
    const char* reason{};
    if (!canonical || !transaction.ready()
        || !build_data::find_item_definition_hash(context.receipt.wrapperHash, wrapper)
        || !stage_action(current, context.action, context.receipt.sourceInstanceSoid,
                         wrapper.definitionIndex, 1, context.bucketSelector,
                         context.hasClock, context.clock, *canonical, reason)
        || canonical->everversePackage != mutation.everversePackage
        || canonical->accountSoid != mutation.accountSoid
        || canonical->characterSoid != mutation.characterSoid
        || canonical->characterIndex != mutation.characterIndex
        || !helpers::same_character(canonical->afterCharacter, mutation.afterCharacter)
        || !helpers::same_profile_views(canonical->afterProfileItems, canonical->afterProfileItemCount,
                                        mutation.afterProfileItems, mutation.afterProfileItemCount)
        || !same_rewards(*canonical, mutation))
        return false;
    after = current;
    after.profileItems = canonical->afterProfileItems;
    after.profileItemCount = canonical->afterProfileItemCount;
    return account::valid(after) && helpers::valid_profile_inventory(after) && transaction.commit();
}

bool commit_package_action(const PendingRecordRewardGrant& mutation) noexcept {
    std::unique_ptr<AccountState> current(new (std::nothrow) AccountState);
    std::unique_ptr<AccountState> after(new (std::nothrow) AccountState);
    store::Transaction transaction;
    if (!current || !after || !transaction.ready() || !store::read_account(*current)
        || !materialize_package_action(*current, mutation, *after))
        return false;
    const auto& context = *mutation.everversePackage;
    return store::write_account(*after)
           && (context.action == PackageAction::refund
               || (write_native_ownership(context.ownership)
                   && record_ownership(current->primarySoid, context.receipt.itemHash)))
           && consume_purchase_receipt(current->primarySoid, context.receipt, context.action)
           && transaction.commit();
}
} // namespace sunrise::state::eververse
