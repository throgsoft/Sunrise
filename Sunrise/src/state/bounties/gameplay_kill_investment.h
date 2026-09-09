#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include "../account/account_state.h"
#include "gameplay_progress.h"
#include "kill_rule_catalog.h"

namespace sunrise::state {

enum class GameplayKillStatus : std::uint8_t {
    applied, acceptedNoChange, invalidContext, invalidEvent, wrongCharacter,
    invalidAccount, duplicate, capacity
};

struct GameplayKillResult final {
    GameplayKillStatus status{GameplayKillStatus::invalidEvent};
    /** Number of objective lanes advanced, including separate held copies. */
    std::size_t applied{};
    std::uint64_t character{};
    std::array<std::uint64_t, 128> instances{};
    std::size_t changedCount{};
    std::size_t triumphsAdvanced{};
};

namespace bounties {

namespace detail {

[[nodiscard]] inline bool weapon_slot_matches(
    const std::optional<gameplay::WeaponSlot>& required, const gameplay::Event& event) noexcept {
    if (!required) return true;
    return gameplay::valid_weapon_slot(*required) && event.weaponKill.value_or(false)
        && event.weaponSlot && *event.weaponSlot == *required
        && !event.meleeKill.value_or(false) && !event.grenadeKill.value_or(false)
        && !event.superKill.value_or(false) && !event.abilityKill.value_or(false);
}

[[nodiscard]] inline bool source_alternatives_match(
    const std::optional<SourceAlternatives>& alternatives, const gameplay::Event& event) noexcept {
    if (!alternatives) return true;
    if (alternatives->weaponSlot && weapon_slot_matches(alternatives->weaponSlot, event)) return true;
    if ((alternatives->grenade && event.grenadeKill.value_or(false))
        || (alternatives->super && event.superKill.value_or(false))
        || (alternatives->precision && event.precision.value_or(false))
        || (alternatives->ability && event.abilityKill.value_or(false))) return true;
    if (!event.weaponKill.value_or(false) || !event.weaponClass || *event.weaponClass == 0)
        return false;
    return std::any_of(alternatives->weaponClasses.begin(), alternatives->weaponClasses.end(),
        [&](std::uint32_t hash) { return hash != 0 && hash == *event.weaponClass; });
}

/** Stages objective changes using the joined host destination, not the character's
 * presentation-only currentActivityIndex. The SQLite caller owns replay admission and commit.
 * Installed definitions resolve each rule's objective ordinal before its predicates run.
 */
[[nodiscard]] inline GameplayKillResult apply_gameplay_kill_transaction(
    AccountState& accountState, const gameplay::Event& event,
    gameplay::Context currentContext, std::uint16_t authoritativeActivity,
    std::span<const KillRuleDefinition> catalog, std::int64_t now,
    bool (*mappingValid)(KillRuleDefinition&) noexcept,
    bool (*validate)(const AccountState&) noexcept) noexcept {
    GameplayKillResult result{};
    if (currentContext.session == 0 || currentContext.generation == 0
        || currentContext.sourceGeneration == 0 || event.context != currentContext
        || authoritativeActivity == 0xFFFFU || !event.activityIndex
        || *event.activityIndex != authoritativeActivity) {
        result.status = GameplayKillStatus::invalidContext;
        return result;
    }
    if (event.kind != gameplay::Kind::authoredKill || event.sequence == 0
        || !event.creditedCharacter || *event.creditedCharacter == 0 || now <= 0) {
        return result;
    }
    if (mappingValid == nullptr || validate == nullptr
        || accountState.characterCount > accountState.characters.size() || !validate(accountState)) {
        result.status = GameplayKillStatus::invalidAccount;
        return result;
    }
    std::size_t characterIndex = accountState.characters.size();
    std::size_t selectedCount = 0;
    for (std::size_t i = 0; i < accountState.characterCount; ++i) {
        if (!accountState.characters[i].selected) continue;
        ++selectedCount;
        if (accountState.characters[i].soid == *event.creditedCharacter) characterIndex = i;
    }
    if (selectedCount != 1 || characterIndex == accountState.characters.size()) {
        result.status = GameplayKillStatus::wrongCharacter;
        return result;
    }
    const auto& inventory = accountState.characters[characterIndex].inventory;
    if (inventory.count > inventory.values.size()) {
        result.status = GameplayKillStatus::invalidAccount;
        return result;
    }
    // The publisher addresses instances, so ambiguous instance identities cannot be staged.
    for (std::size_t i = 0; i < inventory.count; ++i) {
        if (inventory.values[i].instanceSoid == 0) {
            result.status = GameplayKillStatus::invalidAccount;
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (inventory.values[i].instanceSoid == inventory.values[j].instanceSoid) {
                result.status = GameplayKillStatus::invalidAccount;
                return result;
            }
        }
    }
    // Keep the complete account after-image off the simulation thread's stack.
    const auto candidate = std::unique_ptr<AccountState>(new (std::nothrow) AccountState(accountState));
    if (!candidate) {
        result.status = GameplayKillStatus::capacity;
        return result;
    }
    auto& after = candidate->characters[characterIndex].inventory;
    for (std::size_t i = 0; i < inventory.count; ++i) {
        const auto& held = inventory.values[i];
        const auto expiry = held.objectiveValues[account::inventory::kItemExpiryLane];
        std::array<bool, account::inventory::kItemObjectiveCapacity> changedLanes{};
        bool changed = false;
        for (auto rule : catalog) {
            if (rule.itemHash != held.definitionHash || rule.itemHash == 0 || !mappingValid(rule)
                || (rule.nonExpiringQuest ? expiry != 0 : expiry <= now)
                || rule.itemIndex == 0xFFFFU || rule.objectiveIndex == 0xFFFFU
                || rule.objectiveHash == 0 || rule.completion <= 0
                || rule.lane < account::inventory::kItemObjectiveLaneBase
                || rule.lane >= changedLanes.size() || changedLanes[rule.lane]
                || (rule.activityIndex != 0xFFFFU && rule.activityIndex != authoritativeActivity)
                || (rule.raceHash != 0 && (!event.raceHash || *event.raceHash != rule.raceHash))
                || (rule.area && !areas::matches(*rule.area, event.bubble))
                || (rule.victimClassHash != 0 && (!event.victimClassHash
                    || *event.victimClassHash != rule.victimClassHash))
                || !weapon_slot_matches(rule.weaponSlot, event)
                || (rule.victimRanks != 0 && ((rule.victimRanks & ~std::uint8_t{31}) != 0
                    || !event.victimRankHash
                    || (rule.victimRanks & gameplay::rank_bit(*event.victimRankHash)) == 0))
                || (rule.weaponClass != 0 && (!event.weaponKill.value_or(false)
                    || !event.weaponClass || *event.weaponClass != rule.weaponClass))
                || (rule.damage && (!event.damage || *event.damage != *rule.damage))
                || (rule.equippedSubclassAffinity
                    && event.equippedSubclassAffinity != rule.equippedSubclassAffinity)
                || (rule.weaponOnly && !event.weaponKill.value_or(false))
                || (rule.grenadeOnly && !event.grenadeKill.value_or(false))
                || (rule.superOnly && !event.superKill.value_or(false))
                || (rule.meleeOnly && !event.meleeKill.value_or(false))
                || (rule.abilityOnly && !event.abilityKill.value_or(false))
                || (rule.precisionOnly && !event.precision.value_or(false))
                || !source_alternatives_match(rule.anySource, event)
                || rule.contribution <= 0
                || (rule.provisionalRankPoints && (rule.contribution != 1 || rule.precisionContribution
                    || !event.victimRankHash
                    || !gameplay::provisional_rank_points(*event.victimRankHash)))
                || (rule.precisionContribution && (*rule.precisionContribution < rule.contribution
                    || !event.precision.has_value()))) continue;
            const auto before = held.objectiveValues[rule.lane];
            if (before < 0 || before >= rule.completion) continue;
            const auto contribution = rule.provisionalRankPoints
                ? *gameplay::provisional_rank_points(*event.victimRankHash)
                : rule.precisionContribution && event.precision.value_or(false)
                    ? *rule.precisionContribution : rule.contribution;
            // Clamp before addition so large reviewed weights cannot overflow INT32_MAX.
            after.values[i].objectiveValues[rule.lane] = before
                + std::min(contribution, rule.completion - before);
            after.values[i].objectiveDefinitionIndex = rule.itemIndex;
            changedLanes[rule.lane] = true;
            changed = true;
            ++result.applied;
        }
        if (!changed) continue;
        if (result.changedCount == result.instances.size()) {
            return GameplayKillResult{GameplayKillStatus::capacity};
        }
        result.instances[result.changedCount++] = held.instanceSoid;
    }
    if (!validate(*candidate)) return GameplayKillResult{GameplayKillStatus::invalidAccount};
    static_assert(std::is_nothrow_copy_assignable_v<AccountState>);
    if (result.changedCount != 0) accountState = *candidate;
    result.character = *event.creditedCharacter;
    result.status = result.changedCount != 0 ? GameplayKillStatus::applied
                                           : GameplayKillStatus::acceptedNoChange;
    return result;
}

} // namespace detail
} // namespace bounties
} // namespace sunrise::state
