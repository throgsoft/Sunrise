#include "bounty_redemption_runtime.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "../../core/logging/log.h"
#include "../../core/runtime/wall_clock.h"
#include "../build_data/pursuits/pursuit_progress.h"
#include "../investment/store_internal.h"
#include "bounty_reward_policy.h"
#include "dawning_reward_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::bounty {
namespace inventory = account::inventory;
namespace store = investment::store;
namespace items = build_data::items;
namespace {
using Requests = std::array<DirectRecordReward, kRecordRewardGrantCapacity>;

bool append(Requests& requests,
            std::size_t& count,
            std::uint16_t index,
            std::int32_t quantity) noexcept {
    items::Definition definition{};
    items::details::Definition detail{};
    if (quantity <= 0 || !build_data::find_item_definition_index(index, definition)
        || !build_data::find_configured_item_detail(index, detail)
        || detail.definitionIndex != index || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId)
        return false;
    if (detail.instancedDefinitionState == items::details::InstancedDefinitionState::instanced
        && !detail.equipmentSlot.has_value() && detail.objectiveCount == 0
        && !contains(bounty_policy::kArrivalsUmbralEngrams, definition.definitionHash))
        return false;
    if (detail.instancedDefinitionState == items::details::InstancedDefinitionState::stackable)
        for (std::size_t i = 0; i < count; ++i) {
            if (requests[i].itemDefinitionIndex != index) continue;
            if (quantity > (std::numeric_limits<std::int32_t>::max)() - requests[i].quantity)
                return false;
            requests[i].quantity += quantity;
            return true;
        }
    if (count == requests.size()) return false;
    requests[count++] = {index, quantity};
    return true;
}

bool append_hash(Requests& requests,
                 std::size_t& count,
                 std::uint32_t hash,
                 std::int32_t quantity = 1) noexcept {
    items::Definition definition{};
    return build_data::find_item_definition_hash(hash, definition)
           && definition.definitionHash == hash
           && append(requests, count, definition.definitionIndex, quantity);
}

bool rank_credit(RewardMarker marker, std::int32_t quantity, PendingRedemption& pending) noexcept {
    const bool valor = marker == RewardMarker::valorRankPoints;
    const auto hash = valor ? bounty_policy::kValorRankProgressionHash
                            : bounty_policy::kInfamyRankProgressionHash;
    const auto cap = valor ? bounty_policy::kSunriseValorRankSaturationCap
                           : bounty_policy::kSunriseInfamyRankSaturationCap;
    build_data::progressions::Definition definition{};
    if (quantity <= 0 || !build_data::find_progression_definition_hash(hash, definition)
        || definition.definitionHash != hash
        || definition.scope != build_data::progressions::Scope::account
        || definition.definitionIndex >= build_data::progressions::kDefinitionCapacity)
        return false;
    for (std::size_t i = 0; i < pending.rankCount; ++i)
        if (pending.ranks[i].index == definition.definitionIndex) {
            pending.ranks[i].after = saturating_rank_total(pending.ranks[i].after, quantity, cap);
            return true;
        }
    if (pending.rankCount == pending.ranks.size()) return false;
    auto& credit = pending.ranks[pending.rankCount++];
    credit.index = definition.definitionIndex;
    if (!store::read_unlock(store::Bank::accountProgressions, credit.index, credit.before)
        || credit.before < 0)
        return false;
    credit.after = saturating_rank_total(credit.before, quantity, cap);
    return true;
}

bool collect(AccountState& working,
             std::size_t characterIndex,
             const inventory::Item& source,
             const items::details::Definition& detail,
             PendingRedemption& pending,
             Requests& requests,
             std::size_t& count) noexcept {
    bool paysExperience = false, doubleExperience = false;
    const auto cadence = resolve_cadence(source.definitionHash, detail.lifetimeSeconds);
    auto& character = working.characters[characterIndex];
    if (detail.rewardCount > detail.rewards.size()) return false;
    for (std::size_t entry = 0; entry < detail.rewardCount; ++entry) {
        const auto& reward = detail.rewards[entry];
        if (reward.itemIndex == items::details::kUnavailableItemIndex) continue;
        std::uint32_t markerHash{};
        const auto marker = reward_marker(reward.itemIndex, markerHash);
        if (markerHash == 0) return false;
        if (marker == RewardMarker::experience || marker == RewardMarker::doubleExperience) {
            // Multiple XP rows describe one bounty payout, not independent grants.
            paysExperience = true;
            doubleExperience |= marker == RewardMarker::doubleExperience;
            continue;
        }
        if (marker == RewardMarker::valorRankPoints || marker == RewardMarker::infamyRankPoints) {
            if (!rank_credit(marker, reward.quantity, pending)) return false;
            continue;
        }
        if (marker == RewardMarker::clanExperience) {
            core::log::writef(
                core::log::Channel::state,
                core::log::Level::info,
                "ev=bounty_reward policy=SunriseClanExperienceUnavailableSuppressed source=0x%08X",
                source.definitionHash);
            continue;
        }
        if (marker != RewardMarker::none) {
            const auto policy =
                resolve_reward_policy(source.definitionHash, detail.lifetimeSeconds, marker);
            std::uint32_t selected{};
            if (policy.lootPool == LootPoolId::gambitPrimeSynthesizer) {
                const auto before = static_cast<std::uint8_t>(character.gambitPrimeSynthesizerTier);
                if (before == 0 || before > 3) return false;
                character.gambitPrimeSynthesizerTier =
                    static_cast<GambitPrimeSynthesizerTier>((std::min)(before + 1, 3));
                continue;
            }
            if (policy.lootPool == LootPoolId::gambitPrimeRoleHelmet) {
                const auto classIndex = static_cast<std::size_t>(character.characterClass);
                if (classIndex >= 3) return false;
                for (std::size_t role = 0; role < bounty_policy::kGambitPrimeRoleHelmets.size();
                     ++role) {
                    const auto& row = bounty_policy::kGambitPrimeRoleHelmets[role];
                    if (row.markerHash != markerHash) continue;
                    const auto before =
                        static_cast<std::uint8_t>(character.gambitPrimeHelmetTiers[role]);
                    if (before > 3) return false;
                    const auto next = (std::min)(before + 1, 3);
                    selected = row.classTiers[classIndex][next - 1];
                    character.gambitPrimeHelmetTiers[role] =
                        static_cast<GambitPrimeHelmetTier>(next);
                }
            } else if (policy.lootPool == LootPoolId::wernerImperialsSunriseFallback) {
                if (!append_hash(requests,
                                 count,
                                 bounty_policy::kImperialsHash,
                                 bounty_policy::kSunriseWernerImperialsFallback))
                    return false;
                continue;
            } else if (policy.gearCount != 0) {
                if (policy.lootPool != LootPoolId::revelryArmorS11
                    || !revelry_reward(markerHash, character.characterClass, selected)) {
                    const auto seed =
                        mix_seed(working.primarySoid ^ character.soid ^ source.instanceSoid
                                 ^ (static_cast<std::uint64_t>(source.definitionHash) << 32U)
                                 ^ markerHash ^ entry);
                    if (!choose_pool_member(
                            loot_pool(policy.lootPool, character.characterClass), seed, selected))
                        return false;
                }
            }
            if (selected == 0 || !append_hash(requests, count, selected)) return false;
            continue;
        }
        std::uint16_t paidIndex = reward.itemIndex;
        std::int32_t fallback{};
        std::string_view policyName{};
        const bool scaled =
            scaled_reward(cadence, reward.itemIndex, paidIndex, fallback, policyName);
        auto quantity = reward.quantity;
        if (quantity <= 0) {
            if (quantity < 0 || !scaled) return false;
            quantity = fallback;
            core::log::writef(core::log::Channel::state,
                              core::log::Level::info,
                              "ev=bounty_reward policy=%.*s source=0x%08X item=%u quantity=%d",
                              static_cast<int>(policyName.size()),
                              policyName.data(),
                              source.definitionHash,
                              static_cast<unsigned>(paidIndex),
                              quantity);
        }
        if (!append(requests, count, paidIndex, quantity)) return false;
    }
    if (paysExperience) {
        pending.experience = cadence_experience(cadence) * (doubleExperience ? 2 : 1);
        if (pending.experience <= 0) return false;
        pending.seasonalBefore = seasonal_experience();
        if (pending.seasonalBefore < 0
            || pending.experience
                   > (std::numeric_limits<std::int32_t>::max)() - pending.seasonalBefore)
            return false;
    }
    return true;
}

bool stage(const AccountState& current,
           std::uint64_t soid,
           std::int32_t expectedQuantity,
           std::int64_t grantTime,
           PendingRedemption& pending) noexcept {
    pending = {};
    const auto selected = selected_character_index(current);
    if (!account::valid(current) || !valid_profile_inventory(current)
        || selected >= current.characterCount || soid == 0 || expectedQuantity <= 0)
        return false;
    const auto& before = current.characters[selected];
    std::size_t index = before.inventory.count;
    for (std::size_t i = 0; i < before.inventory.count; ++i)
        if (before.inventory.values[i].instanceSoid == soid) {
            index = i;
            break;
        }
    if (index == before.inventory.count) return false;
    const auto source = before.inventory.values[index];
    items::Definition definition{};
    items::details::Definition detail{};
    if (source.quantity != expectedQuantity || (source.flags & inventory::kLockedItemFlag) != 0
        || !build_data::find_item_definition_hash(source.definitionHash, definition)
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != source.definitionHash || detail.bucketId != definition.bucketId
        || detail.objectiveCount == 0 || detail.objectiveCount > detail.objectiveIndices.size()
        || detail.lifetimeSeconds < 0
        || source.objectiveDefinitionIndex != definition.definitionIndex
        || source.quantity > detail.maxStackSize
        || contains(bounty_policy::kWernerTreasureMaps, source.definitionHash))
        return false;
    const auto measured =
        build_data::pursuits::measure(definition.definitionIndex, source.objectiveValues);
    if (!measured.resolved || measured.objectiveCount != detail.objectiveCount) return false;
    const auto now = core::runtime::investment_clock_seconds();
    if (grantTime <= 0 || grantTime > now) return false;
    const auto deadline = source.objectiveValues[inventory::kItemExpiryLane];
    const bool expired = detail.lifetimeSeconds > 0 && (deadline <= 0 || now >= deadline);
    pending.redeemed = !expired && measured.completeCount == measured.objectiveCount;
    auto workingStorage = std::unique_ptr<AccountState>{new (std::nothrow) AccountState(current)};
    if (!workingStorage) return false;
    auto& working = *workingStorage;
    auto& character = working.characters[selected];
    if (source.quantity == 1) {
        for (std::size_t i = index; i + 1 < character.inventory.count; ++i)
            character.inventory.values[i] = character.inventory.values[i + 1];
        character.inventory.values[--character.inventory.count] = {};
    } else {
        if (character.nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
            return false;
        --character.inventory.values[index].quantity;
        character.inventory.values[index].mutationSerial =
            static_cast<std::int32_t>(character.nextInventorySerial++);
    }
    Requests requests{};
    std::size_t count{};
    std::uint32_t mapHash{};
    const bool transition = pending.redeemed && werner_treasure_map(source.definitionHash, mapHash);
    if (transition) {
        if (!append_hash(requests, count, mapHash)) return false;
    } else if (pending.redeemed
               && !collect(working, selected, source, detail, pending, requests, count))
        return false;
    if (!stage_rewards(working, std::span(requests).first(count), pending.grant, soid, grantTime))
        return false;
    if (transition) {
        for (std::size_t i = 0; i < pending.grant.afterCharacter.inventory.count; ++i) {
            auto& item = pending.grant.afterCharacter.inventory.values[i];
            if (item.definitionHash == mapHash && item.instanceSoid != source.instanceSoid
                && item.objectiveValues[inventory::kItemExpiryLane] > 0)
                item.objectiveValues[inventory::kItemExpiryLane] = deadline;
        }
    }
    // Keep survivors' serials and all source objective lanes; only one source unit was consumed.
    pending.grant.beforeCharacter = before;
    pending.sourceInstanceSoid = soid;
    pending.sourceDefinitionHash = source.definitionHash;
    pending.expectedQuantity = expectedQuantity;
    pending.stagedAt = grantTime;
    pending.prepared = true;
    return true;
}

bool materialize(const AccountState& current,
                 const PendingRedemption& pending,
                 AccountState& after) noexcept {
    const auto& grant = pending.grant;
    if (!pending.prepared || !grant.prepared || current.primarySoid != grant.accountSoid
        || grant.claimedRecordIndex != kUnclaimedRecordIndex || grant.dawningDelivery
        || selected_character_index(current) != grant.characterIndex
        || grant.characterIndex >= current.characterCount
        || !same_character(current.characters[grant.characterIndex], grant.beforeCharacter)
        || !same_profile_inventory(current, grant.beforeProfileItems, grant.beforeProfileItemCount))
        return false;
    auto canonicalStorage =
        std::unique_ptr<PendingRedemption>{new (std::nothrow) PendingRedemption};
    if (!canonicalStorage) return false;
    auto& canonical = *canonicalStorage;
    if (!stage(current,
               pending.sourceInstanceSoid,
               pending.expectedQuantity,
               pending.stagedAt,
               canonical)
        || canonical.sourceDefinitionHash != pending.sourceDefinitionHash
        || canonical.redeemed != pending.redeemed || canonical.experience != pending.experience
        || canonical.seasonalBefore != pending.seasonalBefore || canonical.ranks != pending.ranks
        || canonical.rankCount != pending.rankCount
        || !same_character(canonical.grant.afterCharacter, grant.afterCharacter)
        || !same_profile_views(canonical.grant.afterProfileItems,
                               canonical.grant.afterProfileItemCount,
                               grant.afterProfileItems,
                               grant.afterProfileItemCount)
        || canonical.grant.beforeDawning != grant.beforeDawning
        || canonical.grant.afterDawning != grant.afterDawning
        || canonical.grant.rewardCount != grant.rewardCount || !dawning::validate_rewards(grant))
        return false;
    for (std::size_t i = 0; i < grant.rewardCount; ++i) {
        const auto& a = canonical.grant.rewards[i];
        const auto& b = grant.rewards[i];
        if (a.instanceSoid != b.instanceSoid || a.definitionHash != b.definitionHash
            || a.stateIndex != b.stateIndex || a.quantity != b.quantity
            || a.afterQuantity != b.afterQuantity || a.mutationSerial != b.mutationSerial
            || a.inventoryRow != b.inventoryRow || a.kind != b.kind
            || a.appendedProfileResident != b.appendedProfileResident)
            return false;
    }
    after = current;
    after.characters[grant.characterIndex] = grant.afterCharacter;
    after.profileItems = grant.afterProfileItems;
    after.profileItemCount = grant.afterProfileItemCount;
    return account::valid(after) && valid_profile_inventory(after);
}
} // namespace

bool prepare_redemption(std::uint64_t sourceInstanceSoid,
                        std::int32_t expectedQuantity,
                        PendingRedemption& pending) noexcept {
    const std::lock_guard lock(store::g_mutex);
    auto current = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    pending = {};
    return current && store::read_account(*current)
           && stage(*current,
                    sourceInstanceSoid,
                    expectedQuantity,
                    core::runtime::investment_clock_seconds(),
                    pending);
}

bool preview_redemption(const PendingRedemption& pending, AccountState& after) noexcept {
    const std::lock_guard lock(store::g_mutex);
    after = {};
    auto current = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    return current && store::read_account(*current) && materialize(*current, pending, after);
}

bool commit_redemption(PendingRedemption& pending) noexcept {
    const PendingConsumption consume{pending};
    store::Transaction transaction;
    auto current = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    auto after = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    if (!transaction.ready() || !current || !after || !store::read_account(*current)
        || !materialize(*current, pending, *after) || !dawning::write_rewards(pending.grant)
        || !store::write_account(*after))
        return false;
    for (std::size_t i = 0; i < pending.rankCount; ++i)
        if (!store::write_unlock(
                store::Bank::accountProgressions, pending.ranks[i].index, pending.ranks[i].after))
            return false;
    // This is the sole XP grant. A failed nested write rolls back with source consumption;
    // a replay cannot pass the source and exact before-image checks after a successful commit.
    if (pending.experience > 0 && !grant_seasonal_experience(pending.experience)) return false;
    return transaction.commit();
}

bool prepare_redemption_grant(std::uint64_t source,
                              std::int32_t quantity,
                              PendingRecordRewardGrant& grant) noexcept {
    grant = {};
    auto pending = std::unique_ptr<PendingRedemption>{new (std::nothrow) PendingRedemption};
    if (!pending || !prepare_redemption(source, quantity, *pending)) return false;
    grant = pending->grant;
    grant.pursuitRedemption = static_cast<const PursuitRedemptionContext&>(*pending);
    return true;
}

bool materialize_redemption_grant(const AccountState& current,
                                  const PendingRecordRewardGrant& grant,
                                  AccountState& after) noexcept {
    if (!grant.pursuitRedemption || !grant.pursuitRedemption->prepared) return false;
    auto pending = std::unique_ptr<PendingRedemption>{new (std::nothrow) PendingRedemption};
    if (!pending) return false;
    static_cast<PursuitRedemptionContext&>(*pending) = *grant.pursuitRedemption;
    pending->grant = grant;
    pending->grant.pursuitRedemption.reset();
    return materialize(current, *pending, after);
}

bool commit_redemption_grant(const PendingRecordRewardGrant& grant) noexcept {
    if (!grant.pursuitRedemption || !grant.pursuitRedemption->prepared) return false;
    auto pending = std::unique_ptr<PendingRedemption>{new (std::nothrow) PendingRedemption};
    if (!pending) return false;
    static_cast<PursuitRedemptionContext&>(*pending) = *grant.pursuitRedemption;
    pending->grant = grant;
    pending->grant.pursuitRedemption.reset();
    return commit_redemption(*pending);
}
} // namespace sunrise::state::runtime::detail::bounty
