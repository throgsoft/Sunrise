#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/datagen/family4/progression/layout.h"

namespace sunrise::client::hooks::season_xp_toast::detail {

using Entry = middleware::datagen::family4::progression::layout::Entry;
inline constexpr std::uint32_t kPassToast = 0x3F6623C5;
inline constexpr std::uint32_t kPrestigeToast = 0xD711A653;
inline constexpr std::int32_t kRankXp = 100'000;
inline constexpr std::int32_t kPassCap = 9'900'000;

/** Six dwords written by E07510 at alert payload +910, not selected object +910. */
struct Progress {
    std::int32_t beforeXp{}, afterXp{}, beforeCost{}, afterCost{}, beforeRank{}, afterRank{};
    bool operator==(const Progress&) const = default;
};
static_assert(sizeof(Progress) == 24);

struct Pair {
    bool eligible{};
    Progress pass{}, prestige{};
};

/** Accept only a below-cap account update whose mirrored HUD gain can produce a second toast. */
inline Pair paired_update(std::span<const Entry> before, std::span<const Entry> after) noexcept {
    if (before.size() != 127 || after.size() != before.size()) return {};
    std::size_t pass = before.size(), prestige = before.size();
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].definitionIndex != after[i].definitionIndex) return {};
        if (before[i].definitionIndex != 40 && before[i].definitionIndex != 41) continue;
        auto& slot = before[i].definitionIndex == 40 ? pass : prestige;
        if (slot != before.size() || before[i].reserved != after[i].reserved
            || before[i].values[1] != after[i].values[1]
            || before[i].values[2] != after[i].values[2])
            return {};
        slot = i;
    }
    // The original diff must visit the canonical toast first.
    if (pass >= prestige || prestige >= before.size()) return {};
    const auto b = before[pass].values[0], a = after[pass].values[0];
    if (b < 0 || a <= b || a >= kPassCap || a - b < 3'000) return {};
    if (before[prestige].values[0] != b % kRankXp || after[prestige].values[0] != a % kRankXp
        || a % kRankXp - b % kRankXp < 3'000)
        return {};
    // A rank crossing normally decreases the HUD remainder, so it produces no duplicate.
    // A larger award can cross ranks and still increase it; keep both native pass ranks.
    const auto beforeRank = b / kRankXp + 1, afterRank = a / kRankXp + 1;
    return {true,
            {b % kRankXp, a % kRankXp, kRankXp, kRankXp, beforeRank, afterRank},
            {b % kRankXp, a % kRankXp, kRankXp, kRankXp, 0, 0}};
}

/** Native coalescing retains the first before-fields while replacing all three after-fields. */
inline bool covers(const Progress& queued, const Progress& expected) noexcept {
    // Compare the start as (rank, remainder). A toast begun before a rank crossing can cover
    // later gains in the new rank even when its starting remainder is numerically larger.
    const bool startsEarlier =
        queued.beforeRank >= 1 && queued.beforeRank <= expected.beforeRank && queued.beforeXp >= 0
        && queued.beforeXp < kRankXp
        && (queued.beforeRank < expected.beforeRank || queued.beforeXp <= expected.beforeXp);
    return startsEarlier && queued.afterXp == expected.afterXp
           && queued.beforeCost == expected.beforeCost && queued.afterCost == expected.afterCost
           && queued.afterRank == expected.afterRank;
}

} // namespace sunrise::client::hooks::season_xp_toast::detail
