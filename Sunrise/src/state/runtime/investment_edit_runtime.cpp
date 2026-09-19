#include "investment_edit_runtime.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <span>

#include "../../core/runtime/wall_clock.h"
#include "../account/pursuit_hold.h"
#include "../build_data/items/quest_initialization.h"
#include "../build_data/runtime.h"
#include "../build_data/season_pass/season_pass_catalog.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "bounty_reward_policy.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::investment_edit {
namespace {
namespace data = build_data;
namespace inventory = account::inventory;
namespace store = investment::store;

CharacterState* selected(AccountState& account) noexcept {
    for (std::size_t i = 0; i < account.characterCount; ++i)
        if (account.characters[i].selected) return &account.characters[i];
    return nullptr;
}

bool resolve(std::uint16_t index,
             data::items::Definition& item,
             data::items::details::Definition& detail) noexcept {
    return data::find_item_definition_index(index, item)
           && data::find_configured_item_detail(index, detail)
           && item.definitionHash == detail.definitionHash && item.bucketId == detail.bucketId;
}

bool objectives(const data::items::details::Definition& detail,
                std::array<std::int32_t, inventory::kItemObjectiveLaneCount>& thresholds) noexcept {
    if (detail.objectiveCount == 0 || detail.objectiveCount > thresholds.size()) return false;
    for (std::size_t i = 0; i < detail.objectiveCount; ++i) {
        data::objectives::Definition objective{};
        if (!data::find_objective_definition(detail.objectiveIndices[i], objective)
            || objective.completionValue <= 0)
            return false;
        thresholds[i] = objective.completionValue;
    }
    return true;
}

// Match the existing Items allowlist at the authority boundary as well. The UI additionally
// checks the localized bounty type; neither layer treats a missing dummy flag as permission.
const char* editable(const inventory::Item& held,
                     std::uint16_t index,
                     const data::items::details::Definition& detail) noexcept {
    if (held.objectiveDefinitionIndex != index && held.objectiveDefinitionIndex != 0xFFFFU)
        return "held objective definition differs from installed item";
    const auto expiry = held.objectiveValues[inventory::kItemExpiryLane];
    if (detail.lifetimeSeconds > 0 && expiry == 0) return "held timed pursuit has no saved expiry";
    if (expiry != 0 && expiry <= core::runtime::investment_clock_seconds())
        return "held pursuit has expired";
    return nullptr;
}

Result edit(std::uint16_t index,
            std::int32_t value,
            std::uint8_t lane,
            bool completeAll,
            bool completeTarget = false,
            bool bountiesOnly = false,
            std::uint64_t instanceSoid = 0) noexcept {
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    if (!account) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*account) || !account::valid(*account))
        return {false, 0, "investment database unavailable"};
    auto* character = selected(*account);
    if (!character) return {false, 0, "no selected character"};
    data::items::Definition target{};
    if (!completeAll && !data::find_item_definition_index(index, target))
        return {false, 0, "installed item identity unavailable"};
    std::size_t matched = 0, changed = 0, skipped = 0;
    for (std::size_t i = 0; i < character->inventory.count; ++i) {
        auto& held = character->inventory.values[i];
        if (instanceSoid != 0 && held.instanceSoid != instanceSoid) continue;
        if (!completeAll && held.definitionHash != target.definitionHash) continue;
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!data::find_item_definition_hash(held.definitionHash, item)
            || !resolve(item.definitionIndex, item, detail))
            return {false, 0, "held item metadata unavailable; no changes committed"};
        if (!completeAll && item.definitionIndex != index) continue;
        if (completeAll && detail.objectiveCount == 0) continue;
        if (bountiesOnly
            && (detail.bucketId != data::items::kPursuitBucketId || detail.lifetimeSeconds <= 0
                || detail.maxStackSize > 1))
            continue;
        std::array<std::int32_t, inventory::kItemObjectiveLaneCount> thresholds{};
        if (!objectives(detail, thresholds) || lane > detail.objectiveCount)
            return {false, 0, "objective metadata or lane unavailable; no changes committed"};
        bool itemProgress = true;
        for (std::size_t ordinal = 0; ordinal < detail.objectiveCount; ++ordinal) {
            if (lane != 0 && lane != ordinal + inventory::kItemObjectiveLaneBase) continue;
            data::objectives::Definition objective{};
            itemProgress =
                itemProgress
                && data::find_objective_definition(detail.objectiveIndices[ordinal], objective)
                && objective.itemProgress;
        }
        if (!itemProgress) {
            if (!completeAll)
                return {
                    false, 0, "objective uses a shared or unsupported source, not an item lane"};
            ++skipped;
            continue;
        }
        if (const auto* reason = editable(held, item.definitionIndex, detail))
            return {false, 0, reason};
        ++matched;
        auto after = held.objectiveValues;
        for (std::size_t ordinal = 0; ordinal < detail.objectiveCount; ++ordinal) {
            const auto targetLane = ordinal + inventory::kItemObjectiveLaneBase;
            if (lane == 0 || lane == targetLane)
                after[targetLane] = completeAll || completeTarget ? thresholds[ordinal] : value;
        }
        if (after == held.objectiveValues && held.objectiveDefinitionIndex == item.definitionIndex)
            continue;
        // Only the objectives changed, so the row keeps the serial that holds its grid cell.
        // Reissuing it would move a bounty the moment it completed.
        held.objectiveValues = after;
        held.objectiveDefinitionIndex = item.definitionIndex;
        ++changed;
    }
    if (matched == 0 && skipped == 0)
        return {false, 0, "no matching held pursuit on selected character"};
    if (changed != 0 && !store::write_account(*account)) return {false, 0, "account write refused"};
    if (!transaction.commit()) return {false, 0, "transaction commit failed"};
    return {true,
            changed,
            skipped != 0   ? "item objectives committed; shared/unsupported pursuits skipped"
            : changed == 0 ? "already at requested values"
                           : "objective values committed; no rewards claimed"};
}
enum class DropScope { item, pursuits, bounties, engrams, weapons, armor };

bool matches_gear(const data::items::details::Definition& detail, DropScope scope) noexcept {
    if (detail.instancedDefinitionState != data::items::details::InstancedDefinitionState::instanced
        || !detail.equipmentSlot.has_value() || *detail.equipmentSlot < 0)
        return false;
    std::size_t semanticIndex = inventory::kEquipmentSlotCount;
    if (!runtime::detail::semantic_equipment_slot(static_cast<std::uint8_t>(*detail.equipmentSlot),
                                                  semanticIndex))
        return false;
    using Slot = inventory::EquipmentSlot;
    switch (static_cast<Slot>(semanticIndex)) {
    case Slot::kinetic:
    case Slot::energy:
    case Slot::heavy:
        return scope == DropScope::weapons;
    case Slot::helmet:
    case Slot::gauntlets:
    case Slot::chest:
    case Slot::legs:
    case Slot::classItem:
        return scope == DropScope::armor;
    default:
        return false;
    }
}

Result drop(std::uint16_t index, DropScope scope) noexcept {
    data::items::Definition requested{};
    if (scope == DropScope::item && !data::find_item_definition_index(index, requested))
        return {false, 0, "installed item identity unavailable"};
    std::unique_ptr<AccountState> snapshot(new (std::nothrow) AccountState);
    if (!snapshot) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*snapshot) || !account::valid(*snapshot))
        return {false, 0, "investment database unavailable"};
    auto* character = selected(*snapshot);
    if (!character) return {false, 0, "no selected character"};
    std::size_t kept = 0, removed = 0;
    const auto beforeCount = character->inventory.count;
    for (std::size_t i = 0; i < beforeCount; ++i) {
        const auto held = character->inventory.values[i];
        bool remove = held.definitionHash == requested.definitionHash;
        if (scope != DropScope::item) {
            data::items::Definition item{};
            data::items::details::Definition detail{};
            if (!data::find_item_definition_hash(held.definitionHash, item)
                || !resolve(item.definitionIndex, item, detail))
                return {false,
                        0,
                        "held metadata unavailable; use item.drop for a known identity; no changes "
                        "committed"};
            remove = detail.objectiveCount != 0 && !detail.equipmentSlot.has_value();
            if (scope == DropScope::weapons || scope == DropScope::armor) {
                remove = item.definitionHash == held.definitionHash
                         && detail.definitionIndex == item.definitionIndex
                         && matches_gear(detail, scope);
                if (remove) {
                    data::inventory::buckets::Descriptor bucket{};
                    if (!data::find_inventory_bucket_descriptor(detail.bucketId, bucket))
                        return {false, 0, "held gear bucket unavailable; no changes committed"};
                    remove = bucket.bucketId == detail.bucketId
                             && bucket.arraySelector
                                    == data::inventory::buckets::ArraySelector::character
                             && bucket.equipmentSlot == *detail.equipmentSlot;
                }
                // Inventory is unequipped storage; also exclude any equipped identity explicitly.
                for (std::size_t c = 0; remove && c < snapshot->characterCount; ++c)
                    for (const auto& equipped : snapshot->characters[c].equipment.slots)
                        if (equipped && equipped->instanceSoid == held.instanceSoid) remove = false;
            }
            if (scope == DropScope::engrams) {
                // Installed Engrams bucket (hash 375726501). Packages in Consumables are not
                // engrams merely because they can be opened.
                remove = detail.bucketId == 31 && !detail.equipmentSlot.has_value();
                if (remove) {
                    data::inventory::buckets::Descriptor bucket{};
                    if (!data::find_inventory_bucket_descriptor(detail.bucketId, bucket))
                        return {false, 0, "held engram bucket unavailable; no changes committed"};
                    remove =
                        bucket.arraySelector == data::inventory::buckets::ArraySelector::character;
                }
            }
            // Match bounty.page's installed classification, regardless of saved expiry/progress.
            if (scope == DropScope::bounties) {
                remove = remove && detail.bucketId == data::items::kPursuitBucketId
                         && detail.lifetimeSeconds > 0
                         && detail.objectiveCount <= detail.objectiveIndices.size()
                         && detail.maxStackSize <= 1;
                if (remove) {
                    data::inventory::buckets::Descriptor bucket{};
                    if (!data::find_inventory_bucket_descriptor(detail.bucketId, bucket))
                        return {false, 0, "held bounty bucket unavailable; no changes committed"};
                    remove =
                        bucket.arraySelector == data::inventory::buckets::ArraySelector::character;
                }
            }
        }
        if (remove)
            ++removed;
        else
            character->inventory.values[kept++] = held;
    }
    if (removed == 0)
        return {true,
                0,
                scope == DropScope::bounties  ? "no held bounties"
                : scope == DropScope::engrams ? "no held engrams"
                                              : "no matching unequipped residents"};
    // Compact authored storage without inventing new identities or resetting survivor progress.
    for (std::size_t i = kept; i < beforeCount; ++i)
        character->inventory.values[i] = {};
    character->inventory.count = kept;
    if (!store::write_account(*snapshot) || !transaction.commit())
        return {false, 0, "removal transaction failed; no changes committed"};
    return {true,
            removed,
            scope == DropScope::bounties ? "bounties removed; quests preserved; no rewards granted"
            : scope == DropScope::engrams
                ? "engrams removed; no decryption or rewards granted"
                : "unequipped residents removed; no rewards, claims or refunds"};
}

} // namespace

Result grant_item(std::uint16_t index, std::int32_t quantity, std::uint32_t expectedHash) noexcept {
    data::items::Definition item{};
    data::items::details::Definition detail{};
    if (quantity < 1) return {false, 0, "quantity must be positive"};
    if (!resolve(index, item, detail) || (expectedHash != 0 && item.definitionHash != expectedHash))
        return {false, 0, "installed item identity or detail unavailable or changed"};
    const bool instanced = detail.instancedDefinitionState
                           == data::items::details::InstancedDefinitionState::instanced;
    if (!instanced && quantity > (std::max)(1, detail.maxStackSize))
        return {false, 0, "quantity exceeds the installed stack size"};
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    std::unique_ptr<PendingRecordRewardGrant> pending(new (std::nothrow) PendingRecordRewardGrant);
    if (!account || !pending) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*account) || !account::valid(*account))
        return {false, 0, "investment database unavailable"};
    const auto* character = selected(*account);
    if (!character) return {false, 0, "no selected character"};
    if (detail.objectiveCount != 0) {
        std::array<std::int32_t, inventory::kItemObjectiveLaneCount> thresholds{};
        if (!objectives(detail, thresholds)) return {false, 0, "objective thresholds unavailable"};
        if (quantity != 1) return {false, 0, "pursuits require quantity one"};
        for (std::size_t i = 0; i < character->inventory.count; ++i) {
            const auto& held = character->inventory.values[i];
            if (held.definitionHash != item.definitionHash) continue;
            if (const auto* reason = editable(held, index, detail)) return {false, 0, reason};
            return {true, 0, "already held; expiry and progress preserved"};
        }
    }
    // One batch fills the shared reward capacity; larger instanced requests take several.
    // Reward policy owns bucket placement, Postmaster overflow, stacking, and the Dawning
    // balance and pickup queue an ingredient request stages instead of a resident row.
    std::int32_t remaining = instanced ? quantity : 1;
    std::size_t granted = 0;
    while (remaining > 0) {
        const auto count = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::int32_t>(kRecordRewardGrantCapacity)));
        std::array<DirectRecordReward, kRecordRewardGrantCapacity> rewards{};
        std::fill_n(rewards.begin(), count, DirectRecordReward{index, instanced ? 1 : quantity});
        if (!prepare_record_reward_grant(
                std::span(rewards).first(count), kUnclaimedRecordIndex, *pending))
            return {false, 0, "reward policy refused support, ownership, capacity or quantity"};
        if (!commit_record_reward(*pending)) return {false, 0, "grant commit failed; rolled back"};
        granted += count;
        remaining -= static_cast<std::int32_t>(count);
    }
    if (!transaction.commit()) return {false, 0, "transaction commit failed; rolled back"};
    return {true, granted, "granted through the record reward policy"};
}

Result complete_bounties() noexcept {
    return edit(0, 0, 0, true, false, true);
}

Result set_objective_lane(std::uint64_t instanceSoid,
                          std::uint16_t index,
                          std::int32_t value,
                          std::uint8_t lane) noexcept {
    if (instanceSoid == 0 || value < 0 || lane == 0 || lane > inventory::kItemObjectiveLaneCount)
        return {false, 0, "invalid bounty instance, value or lane"};
    return edit(index, value, lane, false, false, true, instanceSoid);
}

Result complete_bounty(std::uint16_t index, std::uint32_t expectedHash) noexcept {
    data::items::Definition item{};
    data::items::details::Definition detail{};
    if (!resolve(index, item, detail) || item.definitionHash != expectedHash
        || detail.bucketId != data::items::kPursuitBucketId || detail.objectiveCount == 0
        || detail.lifetimeSeconds <= 0)
        return {false, 0, "installed expiring bounty unavailable"};
    return edit(index, 0, 0, false, true);
}
Result drop_bounties() noexcept {
    return drop(0, DropScope::bounties);
}
Result drop_engrams() noexcept {
    return drop(0, DropScope::engrams);
}
Result drop_weapons() noexcept {
    return drop(0, DropScope::weapons);
}
Result drop_armor() noexcept {
    return drop(0, DropScope::armor);
}

Result drop_season_pass() noexcept {
    namespace pass = progression::season_pass;
    constexpr std::array progressionIndices{pass::kProgressionDefinitionIndex,
                                            pass::kHudProgressionDefinitionIndex};
    std::array<data::season_pass::Reward, data::season_pass::kRewardCapacity> rewards{};
    std::size_t rewardCount = 0;
    if (!data::season_pass::snapshot(rewards, rewardCount) || rewardCount == 0)
        return {false, 0, "installed season pass rewards unavailable; no changes committed"};

    std::array<std::uint16_t, data::progressions::kDefinitionCapacity> accountSlots{};
    std::size_t slotCount = 0;
    if (!data::find_progression_slots(data::progressions::Scope::account, accountSlots, slotCount))
        return {false, 0, "installed account progression mapping unavailable"};
    std::size_t passRankCount = 0;
    for (const auto index : progressionIndices) {
        std::array<data::progressions::Step, data::progressions::kStepPerDefinitionCapacity>
            steps{};
        std::size_t stepCount = 0;
        if (std::find(accountSlots.begin(), accountSlots.begin() + slotCount, index)
                == accountSlots.begin() + slotCount
            || !data::find_progression_steps(index, steps, stepCount) || stepCount == 0)
            return {false, 0, "installed season pass progression unavailable"};
        for (std::size_t step = 0; step < stepCount; ++step)
            if (steps[step].cost < 0
                || (index == pass::kProgressionDefinitionIndex
                    && (step == 0 ? steps[step].cost != 0 : steps[step].cost == 0)))
                return {false, 0, "installed season pass rank mapping unavailable"};
        if (index == pass::kProgressionDefinitionIndex) passRankCount = stepCount;
    }
    // These are reward-row claims, not item/collectible ownership. Class wrappers have their
    // own rows in this same catalog; expanding a wrapper only grants inventory residents.
    for (std::size_t row = 0; row < rewardCount; ++row) {
        const auto& reward = rewards[row];
        data::items::Definition item{};
        if (reward.claimFlagIndex == data::season_pass::kUnavailableFlagIndex
            || reward.claimFlagIndex >= unlocks::kAccountFlagCapacity || reward.quantity == 0
            || reward.requiredRank > passRankCount
            || !data::find_item_definition_index(reward.itemIndex, item) || reward.itemHash == 0
            || item.definitionHash != reward.itemHash)
            return {false, 0, "season pass reward identity or claim mapping unavailable"};
    }

    std::unique_ptr<unlocks::Table> banks(new (std::nothrow) unlocks::Table);
    if (!banks) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_unlocks(*banks))
        return {false, 0, "investment unlock banks unavailable; no changes committed"};
    for (std::size_t row = 0; row < rewardCount; ++row)
        if (banks->accountFlags[rewards[row].claimFlagIndex] > 3)
            return {false, 0, "invalid native season pass claim value; no changes committed"};

    // Finish preflight before changing even the call-local after-image. Only these cells differ,
    // so write_unlocks persists only their sparse SQLite rows, under this outer transaction.
    std::size_t changed = 0;
    for (std::size_t row = 0; row < rewardCount; ++row) {
        auto& flag = banks->accountFlags[rewards[row].claimFlagIndex];
        if (flag != unlocks::kFlagClear) {
            flag = unlocks::kFlagClear;
            ++changed; // Shared claim indices count once.
        }
    }
    for (const auto index : progressionIndices)
        for (auto& lane : banks->accountProgressions[index])
            if (lane != 0) {
                lane = 0;
                ++changed;
            }
    if ((changed != 0 && !store::write_unlocks(*banks)) || !transaction.commit())
        return {false, 0, "season pass reset transaction failed; no changes committed"};
    return {true,
            changed,
            changed == 0 ? "season pass already reset (zero XP, native rank 1)"
                         : "season pass claims and XP reset to native rank 1; "
                           "items and artifact preserved"};
}

} // namespace sunrise::state::investment_edit
