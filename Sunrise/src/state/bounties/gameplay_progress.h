#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "named_area_rules.h"
#include "weapon_slot.h"

namespace sunrise::state::bounties::gameplay {

/** Semantic values, intentionally independent of native and manifest enum ordering. */
enum class Damage : std::uint8_t { kinetic, solar, arc, voidDamage };

/** Semantic rank selection bits; native labels are hashes, not an ordinal strength enum. */
inline constexpr std::uint8_t kRankBase = 1, kRankBoss = 2, kRankElite = 4, kRankMegaboss = 8,
                              kRankMiniboss = 16;
/** User-adopted ordinary powerful policy: every known non-base rank, one unit per kill. */
inline constexpr std::uint8_t kPowerfulRanks =
    kRankBoss | kRankElite | kRankMegaboss | kRankMiniboss;
constexpr std::uint8_t rank_bit(std::uint32_t hash) noexcept {
    switch (hash) {
    case 0x4CF9B596U:
        return kRankBase;
    case 0x5CFE4C22U:
        return kRankBoss;
    case 0x574459CEU:
        return kRankElite;
    case 0x04F136E0U:
        return kRankMegaboss;
    case 0x9A3D2CCBU:
        return kRankMiniboss;
    default:
        return 0;
    }
}

/** User-selected provisional total points for explicitly rank-weighted bounty lanes.
 * Base and elite earn one, miniboss two, boss three, megaboss four. These are Sunrise
 * policy, not recovered retail multipliers. Ordinary powerful counts stay at one.
 */
constexpr std::optional<std::int32_t> provisional_rank_points(std::uint32_t hash) noexcept {
    switch (rank_bit(hash)) {
    case kRankBase:
    case kRankElite:
        return 1;
    case kRankMiniboss:
        return 2;
    case kRankBoss:
        return 3;
    case kRankMegaboss:
        return 4;
    default:
        return std::nullopt;
    }
}

/** A reviewed adapter must prove finalized death and an eligible enemy victim for
 * confirmedEnemyKill. */
enum class Kind : std::uint8_t {
    unknown,
    populationDecrease,
    predictedDeath,
    confirmedEnemyKill,
    /** Explicitly opted-in bounded experiment; not a confirmed death. */
    experimentalKillCandidate,
    /** Client-authored any_kill with reviewed attribution and victim metadata. */
    authoredKill
};

/** Process-local activity and client generation; none may be inferred from a selected UI row. */
struct Context {
    std::uint64_t session{};
    std::uint64_t generation{};
    std::uint64_t sourceGeneration{};
    friend constexpr bool operator==(const Context&, const Context&) = default;
};

/** Normalized evidence, not a wire layout. Missing facts remain absent. */
struct Event {
    Context context{};
    std::uint64_t sequence{};
    Kind kind{Kind::unknown};
    std::optional<std::uint64_t> creditedCharacter{};
    std::optional<std::uint16_t> activityIndex{};
    std::optional<std::uint32_t> activityTypeHash{};
    std::optional<std::uint32_t> weaponHash{};
    std::optional<std::uint32_t> raceHash{};
    std::optional<bool> precision{};
    std::optional<std::uint32_t> weaponClass{}; // Authored damage-label name hash, not item hash.
    std::optional<Damage> damage{};
    std::optional<bool> weaponKill{};
    std::optional<bool> grenadeKill{};
    std::optional<bool> superKill{};
    // Appended to preserve existing aggregate order; bounty predicates need neither field.
    std::optional<std::uint32_t> abilityLabelHash{};
    std::optional<std::uint32_t> playerClassHash{};
    std::optional<bool> meleeKill{};
    std::optional<std::uint32_t> victimRankHash{};
    std::optional<bool> abilityKill{};
    std::optional<areas::BubbleFact> bubble{};
    /** Selected killing source's equipment slot; does not identify the individual item. */
    std::optional<WeaponSlot> weaponSlot{};
    /** Native actor-profile class at canonical victim+0C, not an entity-definition tag. */
    std::optional<std::uint32_t> victimClassHash{};
    /** Equipped at native observation, independent of killing damage/source attribution. */
    std::optional<Damage> equippedSubclassAffinity{};
};

} // namespace sunrise::state::bounties::gameplay
