#include "dawning_oven_delivery.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../../core/runtime/wall_clock.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "bounty_redemption_runtime.h"
#include "bounty_reward_policy.h"
#include "dawning_bounty_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::dawning {
namespace items = build_data::items;
namespace buckets = build_data::inventory::buckets;
namespace {
bool offer_installed(std::uint32_t vendorHash, std::uint32_t offerHash) noexcept {
    namespace vendors = build_data::vendors;
    vendors::Definition vendor{};
    items::Definition offer{};
    if (!vendors::find(vendorHash, vendor) || vendor.definitionHash != vendorHash
        || !build_data::find_item_definition_hash(offerHash, offer)
        || offer.definitionHash != offerHash)
        return false;
    for (std::size_t i = 0; i < vendor.installedCount; ++i) {
        vendors::InstalledRow installed{};
        if (!vendors::installed_row(vendor, i, installed)) return false;
        if (installed.definitionHash == offerHash) return true;
    }
    for (std::size_t i = 0; i < vendor.saleCount; ++i) {
        vendors::SaleRow sale{};
        if (!vendors::sale_row(vendor, i, sale)) return false;
        if (sale.itemIndex == offer.definitionIndex) return true;
    }
    return false;
}

// Same before-state seeded uniform draw as the working server policy. Refused transactions
// do not advance it; successful item acquisition advances the character's inventory serial.
std::uint32_t select_gear(const AccountState& account, const CharacterState& character) noexcept {
    const auto pool =
        bounty::loot_pool(bounty::LootPoolId::worldLegendaryS11, character.characterClass);
    if (pool.armor.empty()) return 0;
    const std::uint64_t count = pool.universal.size() + pool.armor.size();
    std::uint64_t seed = account.primarySoid ^ character.soid
                         ^ (static_cast<std::uint64_t>(character.nextInventorySerial) << 32U);
    const auto threshold = (0ULL - count) % count;
    std::uint64_t draw{};
    do {
        draw = bounty::next_random(seed);
    } while (draw < threshold);
    const auto index = static_cast<std::size_t>(draw % count);
    return index < pool.universal.size() ? pool.universal[index]
                                         : pool.armor[index - pool.universal.size()];
}
} // namespace

bool stage_delivery(const AccountState& account,
                    std::uint32_t vendorHash,
                    std::uint32_t offerHash,
                    std::int64_t now,
                    PendingRecordRewardGrant& mutation) noexcept {
    mutation = {};
    const auto characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount || now <= 0
        || now > core::runtime::investment_clock_seconds()
        || !offer_installed(vendorHash, offerHash))
        return false;
    std::uint32_t cookieHash{};
    for (const auto& row : kDeliveries)
        if (row.vendorHash == vendorHash && row.offerHash == offerHash) cookieHash = row.cookieHash;
    if (cookieHash == 0) return false;
    items::Definition cookie{}, gear{}, glimmer{};
    items::details::Definition cookieDetail{}, gearDetail{}, glimmerDetail{};
    buckets::Descriptor cookieBucket{}, gearBucket{}, glimmerBucket{};
    const auto gearHash = select_gear(account, account.characters[characterIndex]);
    const auto resolve = [](std::uint32_t hash,
                            items::Definition& item,
                            items::details::Definition& detail,
                            buckets::Descriptor& bucket) noexcept {
        return build_data::find_item_definition_hash(hash, item) && item.definitionHash == hash
               && build_data::find_configured_item_detail(item.definitionIndex, detail)
               && detail.definitionHash == hash && detail.definitionIndex == item.definitionIndex
               && detail.bucketId == item.bucketId && detail.maxStackSize > 0
               && build_data::find_inventory_bucket_descriptor(item.bucketId, bucket);
    };
    if (!resolve(cookieHash, cookie, cookieDetail, cookieBucket)
        || cookieDetail.instancedDefinitionState
               != items::details::InstancedDefinitionState::stackable
        || cookieBucket.arraySelector != buckets::ArraySelector::profile
        || !resolve(gearHash, gear, gearDetail, gearBucket)
        || gearDetail.instancedDefinitionState
               != items::details::InstancedDefinitionState::instanced
        || !gearDetail.equipmentSlot.has_value()
        || gearBucket.arraySelector != buckets::ArraySelector::character
        || !resolve(bounty_policy::kGlimmerHash, glimmer, glimmerDetail, glimmerBucket)
        || glimmerDetail.instancedDefinitionState
               != items::details::InstancedDefinitionState::stackable
        || glimmerBucket.arraySelector != buckets::ArraySelector::profile)
        return false;
    auto chargedStorage = std::unique_ptr<AccountState>{new (std::nothrow) AccountState(account)};
    if (!chargedStorage) return false;
    auto& charged = *chargedStorage;
    std::size_t cookieIndex = charged.profileItemCount;
    std::int32_t serial{};
    for (std::size_t i = 0; i < account.profileItemCount; ++i) {
        const auto& held = account.profileItems[i];
        serial = (std::max)(serial, held.mutationSerial);
        if (held.definitionHash == cookieHash && cookieIndex == account.profileItemCount)
            cookieIndex = i;
    }
    if (cookieIndex == charged.profileItemCount) return false;
    auto& heldCookie = charged.profileItems[cookieIndex];
    if (heldCookie.instanceSoid != 0 || heldCookie.quantity <= 0
        || heldCookie.quantity > cookieDetail.maxStackSize)
        return false;
    if (--heldCookie.quantity != 0) {
        if (serial == (std::numeric_limits<std::int32_t>::max)()) return false;
        heldCookie.mutationSerial = ++serial;
    } else {
        for (std::size_t i = cookieIndex; i + 1 < charged.profileItemCount; ++i)
            charged.profileItems[i] = charged.profileItems[i + 1];
        charged.profileItems[--charged.profileItemCount] = {};
    }
    const std::array<DirectRecordReward, 2> rewards{
        {{gear.definitionIndex, 1}, {glimmer.definitionIndex, 100}}};
    if (!bounty::stage_rewards(charged, rewards, mutation)
        || !credit_bounties(mutation.afterCharacter, true, cookieHash, now))
        return false;
    // Removing the greatest cookie serial must not reuse a serial in reward feedback.
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        auto& reward = mutation.rewards[i];
        if (reward.kind != RecordRewardKind::profileStack) continue;
        if (reward.mutationSerial <= serial) {
            if (serial == (std::numeric_limits<std::int32_t>::max)()) return false;
            reward.mutationSerial = ++serial;
            mutation.afterProfileItems[reward.stateIndex].mutationSerial = reward.mutationSerial;
        } else
            serial = reward.mutationSerial;
    }
    mutation.beforeProfileItems = account.profileItems;
    mutation.beforeProfileItemCount = account.profileItemCount;
    mutation.beforeCharacter = account.characters[characterIndex];
    mutation.dawningDelivery =
        account::inventory::dawning::DeliveryContext{now, vendorHash, offerHash};
    return true;
}

bool materialize_delivery(const AccountState& current,
                          const PendingRecordRewardGrant& mutation,
                          AccountState& after) noexcept {
    if (!mutation.prepared || !mutation.dawningDelivery
        || mutation.claimedRecordIndex != kUnclaimedRecordIndex || mutation.beforeDawning
        || mutation.afterDawning || mutation.characterIndex >= current.characterCount
        || selected_character_index(current) != mutation.characterIndex
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.beforeProfileItemCount))
        return false;
    const auto& context = *mutation.dawningDelivery;
    auto canonicalStorage =
        std::unique_ptr<PendingRecordRewardGrant>{new (std::nothrow) PendingRecordRewardGrant};
    if (!canonicalStorage) return false;
    auto& canonical = *canonicalStorage;
    if (!stage_delivery(current, context.vendorHash, context.offerHash, context.stagedAt, canonical)
        || mutation.accountSoid != canonical.accountSoid
        || mutation.characterSoid != canonical.characterSoid
        || mutation.rewardCount != canonical.rewardCount
        || !same_character(canonical.afterCharacter, mutation.afterCharacter)
        || !same_profile_views(canonical.afterProfileItems,
                               canonical.afterProfileItemCount,
                               mutation.afterProfileItems,
                               mutation.afterProfileItemCount))
        return false;
    for (std::size_t i = 0; i < mutation.rewards.size(); ++i) {
        const auto& a = mutation.rewards[i];
        const auto& b = canonical.rewards[i];
        if (a.instanceSoid != b.instanceSoid || a.definitionHash != b.definitionHash
            || a.stateIndex != b.stateIndex || a.quantity != b.quantity
            || a.afterQuantity != b.afterQuantity || a.mutationSerial != b.mutationSerial
            || a.inventoryRow != b.inventoryRow || a.kind != b.kind
            || a.appendedProfileResident != b.appendedProfileResident)
            return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    after.profileItems = canonical.afterProfileItems;
    after.profileItemCount = canonical.afterProfileItemCount;
    return account::valid(after) && valid_profile_inventory(after);
}
} // namespace sunrise::state::runtime::detail::dawning
