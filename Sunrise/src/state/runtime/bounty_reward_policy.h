#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "bounty_reward_policy_data.h"

namespace sunrise::state::runtime::detail::bounty {
enum class BountyCadence : std::uint8_t {
    unresolved,
    repeatable,
    daily,
    weekly,
};

/** Concrete or explicitly unresolved server loot policy selected for one marker. */
enum class LootPoolId : std::uint8_t {
    none,
    worldLegendaryS11,
    blackArmoryArmor,
    blackArmoryRareS11,
    revelryArmorS11,
    dreamingCity,
    purificationDreamingCity,
    gambitLegacyS11,
    gambitPrimeWeaponsS11,
    gambitPrimeRoleHelmet,
    gambitPrimeSynthesizer,
    ironBannerS11,
    lunasRecallWorldS11,
    invitationsWorldS11,
    lastWish,
    scourgeOfThePast,
    crownOfSorrow,
    trialsOfOsirisS11,
    unresolvedWerner,
    unresolvedWernerGear,
    unresolvedWernerRune,
    wernerImperialsSunriseFallback,
    unresolvedSource,
};

/** Power semantics carried by the display marker or an exact source policy. */
enum class RewardPowerClass : std::uint8_t {
    none,
    legendary,
    powerfulTier1,
    powerfulTier2,
    powerfulTier3,
    pinnacle,
    legacyPowerfulUnknown,
};

struct ScaledReward {
    /** Index the reward row names, which is a display token rather than a currency. */
    std::uint16_t displayIndex{};
    /** Index actually credited. */
    std::uint16_t paidIndex{};
    std::int32_t daily{};
    std::int32_t weekly{};
    /** Stable policy name emitted whenever Sunrise supplies an amount omitted by investment. */
    const char* policyName{};
};

/**
 * Glimmer, then Bright Dust - each named twice in this build, and the reward row names the wrong
 * one.
 *
 * | name | index | bucket | max stack |
 * |---|---|---|---|
 * | Glimmer | 123 | 21 | 250,000 |
 * | Glimmer | **1268** | 28 | **1** |
 * | Bright Dust | 129 | 24 | 999,999 |
 * | Bright Dust | **1271** | 28 | **1** |
 *
 * The pair a reward row names lives in bucket 28 and stacks to one. It is what the client draws in
 * a reward list, not what a wallet holds - the authored starting inventory names 123 and 129, and
 * so does every balance the vendor screens charge against. Crediting the row's own index would mint
 * a single unstackable token beside the real currency and move no balance at all.
 */
constexpr std::array<ScaledReward, 10> kScaledPursuitRewards{{
    {1268, 123, 1'000, 3'000, "SunriseGlimmerCadenceFallback"},
    {123, 123, 1'000, 3'000, "SunriseGlimmerCadenceFallback"},
    // Bungie's target-season economy states the otherwise-zero reward rows explicitly:
    // repeatable bounties pay 10 Bright Dust and weekly bounties pay 200.
    {1271, 129, 10, 200, "ExactS11BrightDustBountyAmounts"},
    // All 21 zero-quantity rows at this index are Banshee daily calibrations; the target-era
    // Gunsmith policy paid one Enhancement Core per daily bounty.
    {1414, 1414, 1, 1, "ExactBansheeDailyEnhancementCore1"},
    {5920, 5920, 10, 25, "SunriseBaryonBoughCadenceFallback"},
    {10741, 10741, 1, 1, "SunriseCadenceFallback"},
    // The target reward rows omit these quantities. Offering and Hymn are singular native
    // consumables. No stronger amount survived for Dark Fragments, so one is named as a Sunrise
    // fallback rather than presented as canonical.
    {12633, 12633, 1, 1, "SunriseDarkFragmentFallback1"},
    {12641, 12641, 1, 1, "ExactOfferingToOracle1"},
    {10502, 10502, 1, 1, "ExactHymnOfDesecration1"},
    {5344, 5344, 1, 1, "ExactPurificationTranscendentBlessing1"},
}};

/** Native semantic reward kinds. Every one is interpreted and never minted into inventory. */
enum class RewardMarker : std::uint8_t {
    none,
    experience,
    doubleExperience,
    legendaryGear,
    powerfulTier1,
    powerfulTier2,
    powerfulTier3,
    pinnacle,
    legendaryRune,
    imperialPurse,
    legacyPowerful,
    valorRankPoints,
    infamyRankPoints,
    clanExperience,
    trialsEngram,
    revelryArms,
    revelryChest,
    revelryHead,
    revelryLegs,
    revelryClassItem,
    gambitPrimeRoleHead,
    gambitPrimeSynthesizerUpgrade,
};

/** Stable marker hashes; Black Armory uses a distinct Legendary Gear definition. */
constexpr std::uint32_t kExperienceRewardHash = 2'211'488'305U;
constexpr std::uint32_t kDoubleExperienceRewardHash = 583'797'518U;
constexpr std::uint32_t kBlackArmoryLegendaryGearHash = 3'407'672'161U;
constexpr std::uint32_t kLegendaryGearHash = 2'127'149'322U;
constexpr std::uint32_t kPowerfulTier1Hash = 3'114'385'605U;
constexpr std::uint32_t kPowerfulTier2Hash = 3'114'385'606U;
constexpr std::uint32_t kPowerfulTier3Hash = 3'114'385'607U;
constexpr std::uint32_t kPinnacleGearHash = 73'143'230U;
constexpr std::uint32_t kLegendaryRuneHash = 1'772'646'107U;
constexpr std::uint32_t kPunyPurseOfImperialsHash = 2'823'823'727U;
constexpr std::uint32_t kLegacyPowerfulGearHash = 4'039'143'015U;

/** Server-authored Shadowkeep-era bounty XP policy. */
constexpr std::int32_t kRepeatableBountyExperience = 4'000;
constexpr std::int32_t kDailyBountyExperience = 6'000;
constexpr std::int32_t kWeeklyBountyExperience = 12'000;

/** A weekly bounty declares seven days; anything past a day and a half is one. */
constexpr std::int32_t kWeeklyLifetimeFloor = 129'600;

/** Authored legacy-rank saturation used only after the marker's exact progression join resolves. */
[[nodiscard]] inline constexpr std::int32_t
saturating_rank_total(std::int32_t before, std::int32_t requested, std::int32_t cap) noexcept {
    if (before >= cap) {
        return before;
    }
    return static_cast<std::int32_t>(
        (std::min)(static_cast<std::int64_t>(before) + requested, static_cast<std::int64_t>(cap)));
}

template <std::size_t Size>
[[nodiscard]] inline constexpr bool contains(const std::array<std::uint32_t, Size>& values,
                                             std::uint32_t wanted) noexcept {
    for (const std::uint32_t value : values) {
        if (value == wanted) {
            return true;
        }
    }
    return false;
}

/** Resolves a native marker by hash, only after its installed definition resolves. */
[[nodiscard]] inline RewardMarker reward_marker(std::uint16_t itemIndex,
                                                std::uint32_t& definitionHash) noexcept {
    build_data::items::Definition definition{};
    definitionHash = build_data::find_item_definition_index(itemIndex, definition)
                         ? definition.definitionHash
                         : 0;
    switch (definitionHash) {
    case kExperienceRewardHash:
        return RewardMarker::experience;
    case kDoubleExperienceRewardHash:
        return RewardMarker::doubleExperience;
    case kBlackArmoryLegendaryGearHash:
    case kLegendaryGearHash:
        return RewardMarker::legendaryGear;
    case kPowerfulTier1Hash:
        return RewardMarker::powerfulTier1;
    case kPowerfulTier2Hash:
        return RewardMarker::powerfulTier2;
    case kPowerfulTier3Hash:
        return RewardMarker::powerfulTier3;
    case kPinnacleGearHash:
        return RewardMarker::pinnacle;
    case kLegendaryRuneHash:
        return RewardMarker::legendaryRune;
    case kPunyPurseOfImperialsHash:
        return RewardMarker::imperialPurse;
    case kLegacyPowerfulGearHash:
        return RewardMarker::legacyPowerful;
    case bounty_policy::kValorRankPointsMarkerHash:
        return RewardMarker::valorRankPoints;
    case bounty_policy::kInfamyRankPointsMarkerHash:
        return RewardMarker::infamyRankPoints;
    case bounty_policy::kClanExperienceMarkerHash:
        return RewardMarker::clanExperience;
    case bounty_policy::kTrialsEngramMarkerHash:
        return RewardMarker::trialsEngram;
    case bounty_policy::kRevelryArmsMarkerHash:
        return RewardMarker::revelryArms;
    case bounty_policy::kRevelryChestMarkerHash:
        return RewardMarker::revelryChest;
    case bounty_policy::kRevelryHeadMarkerHash:
        return RewardMarker::revelryHead;
    case bounty_policy::kRevelryLegsMarkerHash:
        return RewardMarker::revelryLegs;
    case bounty_policy::kRevelryClassItemMarkerHash:
        return RewardMarker::revelryClassItem;
    case bounty_policy::kGambitPrimeSynthesizerUpgradeMarkerHash:
        return RewardMarker::gambitPrimeSynthesizerUpgrade;
    default:
        break;
    }
    if (contains(bounty_policy::kGambitPrimeRoleHeadMarkerHashes, definitionHash)) {
        return RewardMarker::gambitPrimeRoleHead;
    }
    return RewardMarker::none;
}

/** Exact identities take precedence; the lifetime branch is retained only for unaudited families.
 */
[[nodiscard]] inline BountyCadence resolve_cadence(std::uint32_t bountyHash,
                                                   std::int32_t lifetimeSeconds) noexcept {
    if (contains(bounty_policy::kRepeatableBounties, bountyHash)) {
        return BountyCadence::repeatable;
    }
    if (contains(bounty_policy::kDreamingCityDailyBounties, bountyHash)
        || contains(bounty_policy::kTrialsDailyBounties, bountyHash)) {
        return BountyCadence::daily;
    }
    if (contains(bounty_policy::kDreamingCityWeeklyBounties, bountyHash)
        || contains(bounty_policy::kTrialsWeeklyBounties, bountyHash)
        || contains(bounty_policy::kTrialsEndGameFallbackBounties, bountyHash)
        || contains(bounty_policy::kWernerWeeklyBounties, bountyHash)
        || contains(bounty_policy::kHawthorneWorldBounties, bountyHash)
        || contains(bounty_policy::kHawthorneLegacyPowerfulBounties, bountyHash)
        || contains(bounty_policy::kBlackArmoryArmorBounties, bountyHash)
        || contains(bounty_policy::kBlackArmoryRareBounties, bountyHash)
        || contains(bounty_policy::kRevelryWeeklyBounties, bountyHash)
        || contains(bounty_policy::kIronBannerWeeklyBounties, bountyHash)
        || contains(bounty_policy::kLunasRecallWeeklyBounties, bountyHash)
        || contains(bounty_policy::kGambitLegacyBounties, bountyHash)
        || contains(bounty_policy::kGambitPrimeWeaponBounties, bountyHash)
        || contains(bounty_policy::kGambitPrimeRoleWeeklyBounties, bountyHash)
        || contains(bounty_policy::kLastWishBounties, bountyHash)
        || contains(bounty_policy::kScourgeBounties, bountyHash)
        || contains(bounty_policy::kCrownBounties, bountyHash)) {
        return BountyCadence::weekly;
    }
    if (lifetimeSeconds >= kWeeklyLifetimeFloor) {
        return BountyCadence::weekly;
    }
    return lifetimeSeconds > 0 ? BountyCadence::daily : BountyCadence::unresolved;
}

/** Resolves the one exact map that replaces a Werner bounty, if this is a transition stage. */
[[nodiscard]] inline bool werner_treasure_map(std::uint32_t bountyHash,
                                              std::uint32_t& mapHash) noexcept {
    mapHash = 0;
    for (const bounty_policy::WernerTreasureMapTransition& transition :
         bounty_policy::kWernerTreasureMapTransitions) {
        if (transition.bountyHash == bountyHash) {
            mapHash = transition.mapHash;
            return true;
        }
    }
    return false;
}

/** @return Authored XP amount for one resolved cadence, or zero when cadence is unsupported. */
[[nodiscard]] inline constexpr std::int32_t cadence_experience(BountyCadence cadence) noexcept {
    switch (cadence) {
    case BountyCadence::repeatable:
        return kRepeatableBountyExperience;
    case BountyCadence::daily:
        return kDailyBountyExperience;
    case BountyCadence::weekly:
        return kWeeklyBountyExperience;
    default:
        return 0;
    }
}

/** Marker semantics plus the exact source allowlist selected for one bounty. */
struct BountyRewardPolicy {
    BountyCadence cadence{BountyCadence::unresolved};
    LootPoolId lootPool{LootPoolId::none};
    RewardPowerClass powerClass{RewardPowerClass::none};
    std::uint32_t gearCount{};
};

/** Maps a display marker to its semantic power class without inventing a numeric level offset. */
[[nodiscard]] inline constexpr RewardPowerClass marker_power(RewardMarker marker) noexcept {
    switch (marker) {
    case RewardMarker::legendaryGear:
        return RewardPowerClass::legendary;
    case RewardMarker::powerfulTier1:
        return RewardPowerClass::powerfulTier1;
    case RewardMarker::powerfulTier2:
        return RewardPowerClass::powerfulTier2;
    case RewardMarker::powerfulTier3:
        return RewardPowerClass::powerfulTier3;
    case RewardMarker::pinnacle:
        return RewardPowerClass::pinnacle;
    case RewardMarker::legacyPowerful:
        return RewardPowerClass::legacyPowerfulUnknown;
    case RewardMarker::trialsEngram:
        return RewardPowerClass::powerfulTier2;
    case RewardMarker::revelryArms:
    case RewardMarker::revelryChest:
    case RewardMarker::revelryHead:
    case RewardMarker::revelryLegs:
    case RewardMarker::revelryClassItem:
        return RewardPowerClass::legacyPowerfulUnknown;
    default:
        return RewardPowerClass::none;
    }
}

/**
 * Resolves only exact source identities. Unknown markers never fall through to the world pool.
 * Pool membership is evidence-derived; one uniform draw with duplicates is authored policy.
 */
[[nodiscard]] inline BountyRewardPolicy resolve_reward_policy(std::uint32_t bountyHash,
                                                              std::int32_t lifetimeSeconds,
                                                              RewardMarker marker) noexcept {
    BountyRewardPolicy policy{};
    policy.cadence = resolve_cadence(bountyHash, lifetimeSeconds);
    policy.powerClass = marker_power(marker);
    const bool gearMarker = policy.powerClass != RewardPowerClass::none;
    policy.gearCount = gearMarker ? 1U : 0U;
    if (!gearMarker) {
        if (marker == RewardMarker::legendaryRune) {
            policy.lootPool = contains(bounty_policy::kWernerTreasureMaps, bountyHash)
                                  ? LootPoolId::unresolvedWernerRune
                                  : LootPoolId::unresolvedSource;
        } else if (marker == RewardMarker::imperialPurse) {
            policy.lootPool = contains(bounty_policy::kWernerTreasureMaps, bountyHash)
                                  ? LootPoolId::wernerImperialsSunriseFallback
                                  : LootPoolId::unresolvedSource;
        } else if (marker == RewardMarker::clanExperience) {
            // Delivery is an explicit suppression below until a clan-scoped state object exists.
            policy.lootPool = LootPoolId::none;
        } else if (marker == RewardMarker::gambitPrimeRoleHead
                   && contains(bounty_policy::kGambitPrimeRoleWeeklyBounties, bountyHash)) {
            policy.lootPool = LootPoolId::gambitPrimeRoleHelmet;
            policy.powerClass = RewardPowerClass::legacyPowerfulUnknown;
            policy.gearCount = 1;
        } else if (marker == RewardMarker::gambitPrimeSynthesizerUpgrade
                   && contains(bounty_policy::kGambitPrimeRoleWeeklyBounties, bountyHash)) {
            policy.lootPool = LootPoolId::gambitPrimeSynthesizer;
        }
        return policy;
    }
    if (contains(bounty_policy::kHawthorneWorldBounties, bountyHash)) {
        policy.lootPool = LootPoolId::worldLegendaryS11;
    } else if (contains(bounty_policy::kBlackArmoryArmorBounties, bountyHash)) {
        policy.lootPool = LootPoolId::blackArmoryArmor;
    } else if (contains(bounty_policy::kBlackArmoryRareBounties, bountyHash)) {
        policy.lootPool = LootPoolId::blackArmoryRareS11;
    } else if (contains(bounty_policy::kRevelryWeeklyBounties, bountyHash)) {
        policy.lootPool = LootPoolId::revelryArmorS11;
    } else if (contains(bounty_policy::kDreamingCityGearBounties, bountyHash)) {
        policy.lootPool = LootPoolId::dreamingCity;
    } else if (contains(bounty_policy::kPurificationRitualBounties, bountyHash)) {
        policy.lootPool = LootPoolId::purificationDreamingCity;
    } else if (contains(bounty_policy::kGambitLegacyBounties, bountyHash)) {
        policy.lootPool = LootPoolId::gambitLegacyS11;
    } else if (contains(bounty_policy::kIronBannerWeeklyBounties, bountyHash)) {
        policy.lootPool = LootPoolId::ironBannerS11;
    } else if (contains(bounty_policy::kLunasRecallWeeklyBounties, bountyHash)) {
        policy.lootPool = LootPoolId::lunasRecallWorldS11;
        policy.powerClass = RewardPowerClass::legacyPowerfulUnknown;
    } else if (contains(bounty_policy::kInvitationsOfTheNineBounties, bountyHash)) {
        policy.lootPool = LootPoolId::invitationsWorldS11;
    } else if (contains(bounty_policy::kLastWishBounties, bountyHash)) {
        policy.lootPool = LootPoolId::lastWish;
    } else if (contains(bounty_policy::kScourgeBounties, bountyHash)) {
        policy.lootPool = LootPoolId::scourgeOfThePast;
    } else if (contains(bounty_policy::kCrownBounties, bountyHash)) {
        policy.lootPool = LootPoolId::crownOfSorrow;
    } else if (contains(bounty_policy::kHawthorneLegacyPowerfulBounties, bountyHash)) {
        // The marker is Hawthorne's generic Powerful Gear row, not a Menagerie/Calus selector.
        // World membership is reconstructed from the target S11 Legendary Engram preview;
        // uniform selection remains explicitly authored Sunrise policy.
        policy.lootPool = LootPoolId::worldLegendaryS11;
        policy.powerClass = RewardPowerClass::legacyPowerfulUnknown;
    } else if (contains(bounty_policy::kGambitPrimeWeaponBounties, bountyHash)) {
        policy.lootPool = LootPoolId::gambitPrimeWeaponsS11;
    } else if (contains(bounty_policy::kTrialsEndGameFallbackBounties, bountyHash)) {
        policy.lootPool = LootPoolId::trialsOfOsirisS11;
    } else if (contains(bounty_policy::kWernerTreasureMaps, bountyHash)) {
        policy.lootPool = LootPoolId::unresolvedWernerGear;
    } else if (contains(bounty_policy::kWernerBountiesAndMaps, bountyHash)) {
        policy.lootPool = LootPoolId::unresolvedWerner;
    } else {
        policy.lootPool = LootPoolId::unresolvedSource;
    }
    return policy;
}

/** Universal weapons plus the selected class's armor rows for one concrete pool. */
struct LootPoolView {
    std::span<const std::uint32_t> universal{};
    std::span<const std::uint32_t> armor{};
};

template <std::size_t TitanSize, std::size_t HunterSize, std::size_t WarlockSize>
[[nodiscard]] inline std::span<const std::uint32_t>
class_pool(CharacterClass characterClass,
           const std::array<std::uint32_t, TitanSize>& titan,
           const std::array<std::uint32_t, HunterSize>& hunter,
           const std::array<std::uint32_t, WarlockSize>& warlock) noexcept {
    switch (characterClass) {
    case CharacterClass::titan:
        return titan;
    case CharacterClass::hunter:
        return hunter;
    case CharacterClass::warlock:
        return warlock;
    }
    return {};
}

/** Resolves a Revelry wrapper to its exact target-build class/slot armor definition. */
[[nodiscard]] inline bool revelry_reward(std::uint32_t markerHash,
                                         CharacterClass characterClass,
                                         std::uint32_t& definitionHash) noexcept {
    definitionHash = 0;
    const std::size_t classIndex = static_cast<std::size_t>(characterClass);
    if (classIndex >= 3) {
        return false;
    }
    for (const bounty_policy::RevelryArmorRewardRow& row : bounty_policy::kRevelryArmorRewards) {
        if (row.markerHash == markerHash) {
            definitionHash = row.classItems[classIndex];
            return definitionHash != 0;
        }
    }
    return false;
}

/** Returns only concrete authored pools; every unresolved enum produces an empty view. */
[[nodiscard]] inline LootPoolView loot_pool(LootPoolId id, CharacterClass characterClass) noexcept {
    switch (id) {
    case LootPoolId::worldLegendaryS11:
        return {bounty_policy::kWorldWeapons,
                class_pool(characterClass,
                           bounty_policy::kWorldTitanArmor,
                           bounty_policy::kWorldHunterArmor,
                           bounty_policy::kWorldWarlockArmor)};
    case LootPoolId::blackArmoryArmor:
        return {{},
                class_pool(characterClass,
                           bounty_policy::kBlackArmoryTitanArmor,
                           bounty_policy::kBlackArmoryHunterArmor,
                           bounty_policy::kBlackArmoryWarlockArmor)};
    case LootPoolId::blackArmoryRareS11:
        return {bounty_policy::kBlackArmoryForgeWeapons,
                class_pool(characterClass,
                           bounty_policy::kBlackArmoryTitanArmor,
                           bounty_policy::kBlackArmoryHunterArmor,
                           bounty_policy::kBlackArmoryWarlockArmor)};
    case LootPoolId::revelryArmorS11:
        return {{},
                class_pool(characterClass,
                           bounty_policy::kRevelryTitanArmor,
                           bounty_policy::kRevelryHunterArmor,
                           bounty_policy::kRevelryWarlockArmor)};
    case LootPoolId::dreamingCity:
    case LootPoolId::purificationDreamingCity:
        return {bounty_policy::kDreamingCityWeapons,
                class_pool(characterClass,
                           bounty_policy::kDreamingCityTitanArmor,
                           bounty_policy::kDreamingCityHunterArmor,
                           bounty_policy::kDreamingCityWarlockArmor)};
    case LootPoolId::gambitLegacyS11:
        return {bounty_policy::kGambitWeapons,
                class_pool(characterClass,
                           bounty_policy::kGambitTitanArmor,
                           bounty_policy::kGambitHunterArmor,
                           bounty_policy::kGambitWarlockArmor)};
    case LootPoolId::gambitPrimeWeaponsS11:
        return {bounty_policy::kGambitPrimeWeapons, {}};
    case LootPoolId::ironBannerS11:
        return {bounty_policy::kIronBannerWeapons,
                class_pool(characterClass,
                           bounty_policy::kIronBannerTitanArmor,
                           bounty_policy::kIronBannerHunterArmor,
                           bounty_policy::kIronBannerWarlockArmor)};
    case LootPoolId::lunasRecallWorldS11:
    case LootPoolId::invitationsWorldS11:
        return {bounty_policy::kWorldWeapons,
                class_pool(characterClass,
                           bounty_policy::kWorldTitanArmor,
                           bounty_policy::kWorldHunterArmor,
                           bounty_policy::kWorldWarlockArmor)};
    case LootPoolId::lastWish:
        return {bounty_policy::kLastWishWeapons,
                class_pool(characterClass,
                           bounty_policy::kLastWishTitanArmor,
                           bounty_policy::kLastWishHunterArmor,
                           bounty_policy::kLastWishWarlockArmor)};
    case LootPoolId::scourgeOfThePast:
        return {bounty_policy::kScourgeWeapons,
                class_pool(characterClass,
                           bounty_policy::kScourgeTitanArmor,
                           bounty_policy::kScourgeHunterArmor,
                           bounty_policy::kScourgeWarlockArmor)};
    case LootPoolId::crownOfSorrow:
        return {bounty_policy::kCrownWeapons,
                class_pool(characterClass,
                           bounty_policy::kCrownTitanArmor,
                           bounty_policy::kCrownHunterArmor,
                           bounty_policy::kCrownWarlockArmor)};
    case LootPoolId::trialsOfOsirisS11:
        return {bounty_policy::kTrialsWeapons,
                class_pool(characterClass,
                           bounty_policy::kTrialsTitanArmor,
                           bounty_policy::kTrialsHunterArmor,
                           bounty_policy::kTrialsWarlockArmor)};
    default:
        return {};
    }
}

/** SplitMix64 seed finalizer and stream step, used for deterministic prepare/commit re-derivation.
 */
[[nodiscard]] inline constexpr std::uint64_t mix_seed(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] inline constexpr std::uint64_t next_random(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    return mix_seed(state);
}

/** Uniformly selects one eligible member without relying on process-global RNG state. */
[[nodiscard]] inline bool choose_pool_member(const LootPoolView& pool,
                                             std::uint64_t seed,
                                             std::uint32_t& definitionHash) noexcept {
    definitionHash = 0;
    const std::size_t count = pool.universal.size() + pool.armor.size();
    if (count == 0) {
        return false;
    }
    std::uint64_t state = seed;
    const std::uint64_t bound = static_cast<std::uint64_t>(count);
    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)()
                                - (std::numeric_limits<std::uint64_t>::max)() % bound;
    std::uint64_t random = 0;
    do {
        random = next_random(state);
    } while (random >= limit);
    const std::size_t index = static_cast<std::size_t>(random % bound);
    definitionHash = index < pool.universal.size() ? pool.universal[index]
                                                   : pool.armor[index - pool.universal.size()];
    return definitionHash != 0;
}

/**
 * @param cadence Exact or bounded-fallback cadence of the redeemed pursuit.
 * @param itemIndex Reward slot's item.
 * @return What to pay for a slot the content left at zero, or zero when nothing is chosen.
 */
[[nodiscard]] inline bool scaled_reward(BountyCadence cadence,
                                        std::uint16_t itemIndex,
                                        std::uint16_t& paidIndex,
                                        std::int32_t& quantity,
                                        std::string_view& policyName) noexcept {
    for (const ScaledReward& scaled : kScaledPursuitRewards) {
        if (scaled.displayIndex != itemIndex) {
            continue;
        }
        paidIndex = scaled.paidIndex;
        quantity = cadence == BountyCadence::weekly ? scaled.weekly : scaled.daily;
        policyName = scaled.policyName != nullptr ? scaled.policyName : "";
        return true;
    }
    return false;
}

} // namespace sunrise::state::runtime::detail::bounty
