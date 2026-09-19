/** Seasonal XP, Season pass claims and seasonal artifact ownership, all held in State. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>

#include "../build_data/runtime.h"
#include "../investment/investment.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_runtime.h"
#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace {

namespace pass = progression::season_pass;

/** Global unlock value slot the client reads as the artifact Power bonus. */
constexpr std::uint16_t kArtifactPowerBonusSlot = 602;
/** Global unlock value slot the client reads as artifact points spent. */
constexpr std::uint16_t kArtifactPointsUsedSlot = 604;
/** Global unlock value slot the client reads as artifact points earned. */
constexpr std::uint16_t kArtifactPointsEarnedSlot = 605;
/** Character object value row whose destination is the artifact points-spent slot. */
constexpr std::uint16_t kArtifactPointsUsedCharacterRow = 38;

/** One rank of the Season of Arrivals pass costs this much XP. */
constexpr std::int32_t kExperiencePerRank = 100'000;
/** The pass stops at rank 100; XP past it feeds the repeating HUD bar instead. */
constexpr std::uint16_t kMaximumRank = 100;
constexpr std::int32_t kMaximumPassExperience =
    (static_cast<std::int32_t>(kMaximumRank) - 1) * kExperiencePerRank;

/**
 * Artifact points a mod column needs before its rows unlock, by column.
 * TODO: extract; no package field carries the column requirement.
 */
constexpr std::array<std::uint16_t, 5> kArtifactColumnTiers{0, 1, 4, 7, 10};
/** Sale rows one artifact mod column offers. */
constexpr std::uint16_t kArtifactColumnRows = 5;
/** Vendor rows before the first mod column, which are the reset row and its neighbours. */
constexpr std::uint16_t kArtifactFirstColumnRow = 6;

using SaleRows = std::array<build_data::ArtifactSaleRow, build_data::kArtifactSaleRowCapacity>;

[[nodiscard]] std::uint16_t column_tier(std::uint16_t saleIndex) noexcept {
    if (saleIndex < kArtifactFirstColumnRow) {
        return kArtifactColumnTiers[0];
    }
    const std::uint16_t column =
        static_cast<std::uint16_t>((saleIndex - kArtifactFirstColumnRow) / kArtifactColumnRows);
    return column < kArtifactColumnTiers.size() ? kArtifactColumnTiers[column]
                                                : kArtifactColumnTiers.back();
}

/** Walks one progression ladder and counts the ranks the given XP pays for. */
[[nodiscard]] std::uint16_t ladder_ranks(std::uint16_t definitionIndex,
                                         std::int32_t experience) noexcept {
    std::array<build_data::progressions::Step, build_data::progressions::kStepPerDefinitionCapacity>
        steps{};
    std::size_t stepCount = 0;
    if (!build_data::find_progression_steps(definitionIndex, steps, stepCount)) {
        return 0;
    }
    std::int64_t remaining = (std::max)(experience, 0);
    std::uint16_t ranks = 0;
    for (std::size_t step = 0; step < stepCount; ++step) {
        if (remaining < steps[step].cost) {
            break;
        }
        remaining -= steps[step].cost;
        ++ranks;
    }
    return ranks;
}

/** The ladder's first step costs nothing, so a zero total still pays for bonus 1. */
[[nodiscard]] std::uint16_t artifact_power_bonus_for(std::int32_t experience) noexcept {
    return ladder_ranks(kArtifactPowerProgressionIndex, experience);
}

[[nodiscard]] std::uint16_t artifact_points_earned_for(std::int32_t experience) noexcept {
    return ladder_ranks(kArtifactUnlockProgressionIndex, experience);
}

/**
 * Sets one global unlock value override, replacing any existing entry for the slot.
 * @return False when the slot is new and the override list is full.
 */
[[nodiscard]] bool
upsert_value(Family5State& family, std::uint16_t slot, std::int32_t value) noexcept {
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == slot) {
            family.values[index].value = value;
            return true;
        }
    }
    if (family.valueCount >= family.values.size()) {
        return false;
    }
    family.values[family.valueCount++] = UnlockValueOverride{slot, value};
    return true;
}

[[nodiscard]] bool family5_flag_set(const Family5State& family, std::uint16_t slot) noexcept {
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        if (family.flags[index].slot == slot) {
            return family.flags[index].value == unlocks::kFlagSet;
        }
    }
    return false;
}

[[nodiscard]] bool sale_rows(SaleRows& rows, std::size_t& count) noexcept {
    count = 0;
    return build_data::artifact_sale_rows(rows, count) && count != 0;
}

/** @return One bit per sale row an authored family-5 override marks owned. Read at seed only. */
[[nodiscard]] std::uint32_t authored_artifact_mask_locked(const Family5State& family,
                                                          const SaleRows& rows,
                                                          std::size_t count) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t row = 0; row < count && row < 32; ++row) {
        if (rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
            && family5_flag_set(family, rows[row].unlockFlagSlot)) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/** @return One bit per owned artifact sale row, read from the character acquired-flag bank. */
[[nodiscard]] std::uint32_t artifact_mask(const SaleRows& rows, std::size_t count) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t row = 0; row < count && row < 32; ++row) {
        const std::uint16_t mapped = rows[row].characterFlagIndex;
        if (mapped != build_data::collectibles::kUnavailableFlagIndex
            && unlocks::character_object_flag_set(mapped)) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/**
 * Removes the family-5 flag rows that name artifact sale slots.
 * The character bank carries ownership, and a family-5 copy would mask it and cost 25 rows.
 * @param family Global override object, mutated in place.
 * @param rows Artifact sale rows.
 * @param count Rows in use.
 */
void strip_artifact_flags_locked(Family5State& family,
                                 const SaleRows& rows,
                                 std::size_t count) noexcept {
    std::size_t write = 0;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const UnlockFlagOverride flag = family.flags[index];
        bool artifact = false;
        for (std::size_t row = 0; row < count && row < 32 && !artifact; ++row) {
            artifact = rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
                       && rows[row].unlockFlagSlot == flag.slot;
        }
        if (!artifact) {
            family.flags[write++] = flag;
        }
    }
    for (std::size_t index = write; index < family.flagCount; ++index) {
        family.flags[index] = {};
    }
    family.flagCount = write;
}

[[nodiscard]] std::uint16_t points_used(std::uint32_t mask) noexcept {
    std::uint16_t used = 0;
    for (std::uint16_t bit = 0; bit < 32; ++bit) {
        used = static_cast<std::uint16_t>(used + ((mask & (1U << bit)) != 0 ? 1 : 0));
    }
    return used;
}

/** Character-bank write of an artifact publish; the family-5 overrides carry only the counters. */
struct CharacterArtifactWrite {
    const SaleRows* rows{};
    std::size_t count{};
    std::uint32_t mask{};
    std::uint16_t used{};
};

/**
 * Writes the artifact ownership bits and the points-used counter into the character banks.
 * @param write Mapped sale rows with the mask and the used count to publish.
 */
bool publish_artifact_character_banks(CharacterArtifactWrite& write) noexcept {
    return unlocks::mutate(&write, [](void* context, unlocks::Table& table) noexcept {
        const auto& request = *static_cast<const CharacterArtifactWrite*>(context);
        for (std::size_t row = 0; row < request.count && row < 32; ++row) {
            const std::uint16_t mapped = (*request.rows)[row].characterFlagIndex;
            if (mapped == build_data::collectibles::kUnavailableFlagIndex
                || mapped >= table.characterObjectFlags.size()) {
                continue;
            }
            table.characterObjectFlags[mapped] =
                (request.mask & (1U << row)) != 0 ? unlocks::kFlagSet : unlocks::kFlagClear;
        }
        if (kArtifactPointsUsedCharacterRow < table.characterObjectValues.size()) {
            table.characterObjectValues[kArtifactPointsUsedCharacterRow] = request.used;
        }
    });
}

/**
 * Writes one artifact ownership mask into the character bank and its counters into family 5.
 * @param family Global override object, mutated in place.
 * @param mask One bit per owned sale row.
 * @param experience Seasonal XP the derived counters are computed from.
 * @return False only when the bounded value override list is full.
 */
[[nodiscard]] bool publish_artifact_locked(Family5State& family,
                                           std::uint32_t mask,
                                           std::int32_t experience) noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    strip_artifact_flags_locked(family, rows, count);
    const std::uint16_t used = points_used(mask);
    if (!upsert_value(family, kArtifactPowerBonusSlot, artifact_power_bonus_for(experience))
        || !upsert_value(family, kArtifactPointsUsedSlot, used)
        || !upsert_value(
            family, kArtifactPointsEarnedSlot, artifact_points_earned_for(experience))) {
        return false;
    }
    CharacterArtifactWrite write{&rows, count, mask, used};
    return publish_artifact_character_banks(write);
}

/** Reconstructs pass XP from its capped ladder and post-cap HUD overflow, independently of
 * artifact XP. Hold the database lock across both reads so a grant/reset cannot split them.
 */
[[nodiscard]] bool read_pass_experience(std::int32_t& experience) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    experience = 0;
    std::int32_t ladder = 0, hud = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::accountProgressions, pass::kProgressionDefinitionIndex, ladder)
        || !investment::store::read_unlock(
            investment::store::Bank::accountProgressions, pass::kHudProgressionDefinitionIndex, hud)
        || ladder < 0 || hud < 0) {
        return false;
    }
    // A ladder written above the cap by an older build must not block every later grant; the
    // publication path clamps to the same bound, so reading it back clamped is the stored value.
    ladder = (std::min)(ladder, kMaximumPassExperience);
    const auto overflow = ladder == kMaximumPassExperience ? hud : 0;
    if (overflow > (std::numeric_limits<std::int32_t>::max)() - ladder) {
        return false;
    }
    experience = ladder + overflow;
    return true;
}

/** Pass and artifact XP normally grow together, but a pass-only reset must stay independent. */
bool publish_experience_lanes(std::int32_t artifactExperience,
                              std::int32_t passExperience) noexcept {
    return unlocks::set_account_progression(kArtifactPowerProgressionIndex, artifactExperience)
           && unlocks::set_account_progression(kArtifactUnlockProgressionIndex, artifactExperience)
           && unlocks::set_account_progression(pass::kProgressionDefinitionIndex,
                                               (std::min)(passExperience, kMaximumPassExperience))
           && unlocks::set_account_progression(pass::kHudProgressionDefinitionIndex,
                                               passExperience < kMaximumPassExperience
                                                   ? passExperience % kExperiencePerRank
                                                   : passExperience - kMaximumPassExperience);
}

} // namespace

/** @return Saved pass XP from account progressions 40/41, including post-rank-100 overflow. */
std::int32_t seasonal_experience() noexcept {
    std::int32_t experience = 0;
    return read_pass_experience(experience) ? experience : 0;
}

/** Republishes each ladder from its own saved XP, preserving a pass-only reset on sign-in. */
bool seed_seasonal_progression() noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    std::int32_t artifactExperience = 0, passExperience = 0;
    if (!transaction.ready() || !investment::store::read_family5(family)
        || !investment::store::read_unlock(investment::store::Bank::accountProgressions,
                                           kArtifactPowerProgressionIndex,
                                           artifactExperience)
        || artifactExperience < 0 || !read_pass_experience(passExperience)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    // An authored family-5 row still seeds ownership. The publish moves it to the character bank.
    const std::uint32_t authoredMask = authored_artifact_mask_locked(family, rows, count);
    if (authoredMask != 0) {
        const AccountState account = investment::store::account();
        for (std::size_t character = 0; character < account.characterCount; ++character) {
            unlocks::Table banks;
            if (!investment::store::read_unlocks(banks, static_cast<int>(character))) {
                investment::store::g_mutex.unlock();
                return false;
            }
            for (std::size_t row = 0; row < count && row < 32; ++row) {
                const auto flag = rows[row].characterFlagIndex;
                if ((authoredMask & (1U << row)) != 0 && flag < banks.characterObjectFlags.size()) {
                    banks.characterObjectFlags[flag] = unlocks::kFlagSet;
                }
            }
            if (!investment::store::write_unlocks(banks, static_cast<int>(character))) {
                investment::store::g_mutex.unlock();
                return false;
            }
        }
    }
    const std::uint32_t mask = artifact_mask(rows, count);
    const bool published = publish_experience_lanes(artifactExperience, passExperience)
                           && publish_artifact_locked(family, mask, artifactExperience)
                           && investment::store::write_family5(family) && transaction.commit();
    investment::store::g_mutex.unlock();
    return published;
}

/** @return The native pass ladder's rank; its zero-cost first step makes zero XP rank 1. */
std::uint16_t seasonal_rank() noexcept {
    return ladder_ranks(pass::kProgressionDefinitionIndex,
                        unlocks::account_progression(pass::kProgressionDefinitionIndex));
}

/** @return Account-wide Power bonus published by the seasonal artifact. */
std::uint16_t artifact_power_bonus() noexcept {
    investment::store::g_mutex.lock();
    Family5State family;
    (void)investment::store::read_family5(family);
    std::int32_t bonus = 0;
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == kArtifactPowerBonusSlot) {
            bonus = family.values[index].value;
            break;
        }
    }
    investment::store::g_mutex.unlock();
    return bonus > 0 ? static_cast<std::uint16_t>(bonus) : 0U;
}

/** Adds base XP to the seasonal lanes and republishes every value derived from the total. */
bool grant_seasonal_experience(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    std::int32_t previous = 0, passPrevious = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::accountProgressions, kArtifactPowerProgressionIndex, previous)
        || previous < 0 || !read_pass_experience(passPrevious)
        || previous > (std::numeric_limits<std::int32_t>::max)() - amount
        || passPrevious > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    const std::int32_t total = previous + amount;

    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        return false;
    }
    SaleRows rows{};
    std::size_t count = 0;
    const std::uint32_t mask = sale_rows(rows, count) ? artifact_mask(rows, count) : 0U;
    const bool saved = publish_experience_lanes(total, passPrevious + amount)
                       && publish_artifact_locked(family, mask, total)
                       && investment::store::write_family5(family) && transaction.commit();
    return saved;
}

/** @return True when this Season pass reward row is already claimed. */
bool season_pass_reward_claimed(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    return build_data::find_season_pass_reward(rewardIndex, reward)
           && reward.claimFlagIndex != build_data::season_pass::kUnavailableFlagIndex
           && unlocks::account_flag_set(reward.claimFlagIndex);
}

/** Claims one Season pass reward row into the account flag its row names. */
bool claim_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)
        || reward.claimFlagIndex == build_data::season_pass::kUnavailableFlagIndex
        || unlocks::account_flag_set(reward.claimFlagIndex)) {
        return false;
    }
    return unlocks::set_account_flag(reward.claimFlagIndex, unlocks::kFlagSet);
}

/** Undoes one Season pass claim so a refused commit cannot leave it held. */
void revoke_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    if (build_data::find_season_pass_reward(rewardIndex, reward)
        && reward.claimFlagIndex != build_data::season_pass::kUnavailableFlagIndex) {
        (void)unlocks::set_account_flag(reward.claimFlagIndex, unlocks::kFlagClear);
    }
}

/** @return Purchased artifact sale rows, one bit per row. */
std::uint32_t artifact_mod_mask() noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return 0;
    }
    return artifact_mask(rows, count);
}

/** Replaces the exact published mask, refusing when another action changed it first. */
bool replace_artifact_mod_mask(std::uint32_t expected, std::uint32_t replacement) noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    const std::int32_t experience = unlocks::account_progression(kArtifactPowerProgressionIndex);
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    bool replaced = artifact_mask(rows, count) == expected;
    if (replaced) {
        replaced = publish_artifact_locked(family, replacement, experience)
                   && investment::store::write_family5(family) && transaction.commit();
    }
    investment::store::g_mutex.unlock();
    return replaced;
}

/** Writes one affordable artifact purchase and keeps its before-image for the commit. */
bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                 PendingArtifactPurchase& mutation) noexcept {
    mutation = {};
    SaleRows rows{};
    std::size_t count = 0;
    const AccountState account = account_snapshot();
    if (!sale_rows(rows, count) || saleIndex >= count || saleIndex >= 32
        || rows[saleIndex].unlockFlagSlot == build_data::collectibles::kUnavailableFlagSlot
        || !account::valid(account)) {
        return false;
    }
    std::size_t selected = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selected = index;
            break;
        }
    }
    if (selected >= account.characterCount) {
        return false;
    }
    const std::uint32_t bit = 1U << saleIndex;
    const std::int32_t experience = unlocks::account_progression(kArtifactPowerProgressionIndex);
    const std::uint16_t earned = artifact_points_earned_for(experience);
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    const std::uint32_t before = artifact_mask(rows, count);
    const std::uint16_t used = points_used(before);
    bool prepared = (before & bit) == 0 && used < earned && used >= column_tier(saleIndex);
    if (prepared) {
        prepared = publish_artifact_locked(family, before | bit, experience)
                   && investment::store::write_family5(family) && transaction.commit();
    }
    investment::store::g_mutex.unlock();
    if (!prepared) {
        return false;
    }
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[selected].soid;
    mutation.characterIndex = selected;
    mutation.beforeMask = before;
    mutation.afterMask = before | bit;
    mutation.saleIndex = saleIndex;
    mutation.prepared = true;
    return true;
}

/** Keeps a prepared purchase only while its character and mask are still the prepared ones. */
bool commit_artifact_mod_unlock(PendingArtifactPurchase& mutation) noexcept {
    const PendingArtifactPurchase prepared = mutation;
    mutation = {};
    const AccountState account = account_snapshot();
    return prepared.prepared && prepared.accountSoid != 0 && prepared.characterSoid != 0
           && prepared.beforeMask != prepared.afterMask
           && prepared.characterIndex < account.characterCount
           && account.primarySoid == prepared.accountSoid
           && account.characters[prepared.characterIndex].soid == prepared.characterSoid
           && account.characters[prepared.characterIndex].selected
           && artifact_mod_mask() == prepared.afterMask;
}

} // namespace sunrise::state
