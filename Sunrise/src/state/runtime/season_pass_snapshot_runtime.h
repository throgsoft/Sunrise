#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../build_data/season_pass/definition.h"

namespace sunrise::state {

/** One native pass row and its saved state, evaluated without a draw. */
struct SeasonPassRowStatus {
    /** Installed item the row grants; zero for an unavailable placeholder row. */
    std::uint32_t itemHash{};
    std::uint32_t quantity{};
    std::uint8_t requiredRank{};
    bool available{};
    bool claimed{};
    /** The granted item's account acquisition flag is set. */
    bool acquired{};
    /** The earned-perk walk grants this row, so the player never claims it. */
    bool automatic{};
    bool rankMet{};
    /** The row condition was evaluated against the selected character's banks. */
    bool conditionKnown{};
    bool conditionMet{};
};

/** One coherent saved view of the Season pass. */
struct SeasonPassSnapshot {
    std::uint64_t accountSoid{};
    /** Selected character the conditions were evaluated for, or zero when none is selected. */
    std::uint64_t characterSoid{};
    std::uint64_t investmentRevision{};
    std::int32_t experience{};
    std::uint16_t rank{};
    /** Catalog rows, including unavailable placeholders; the index is the native claim index. */
    std::size_t rowCount{};
    std::array<SeasonPassRowStatus, build_data::season_pass::kRewardCapacity> rows{};
};

/**
 * Reads XP, claims and the selected character's banks under one store lock, then evaluates every
 * pass row against those copies. Never writes and never draws.
 * @param output Receives the snapshot; cleared on any refusal.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool read_season_pass_snapshot(SeasonPassSnapshot& output,
                                             const char** refusal = nullptr) noexcept;

/**
 * Applies the opcode-2400 claim's rules that need no draw, so a claim can be refused before it
 * is queued.
 * @param rewardIndex Native reward-array index.
 * @param itemHash Item the row is expected to grant.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool season_pass_reward_claimable(std::uint16_t rewardIndex,
                                                std::uint32_t itemHash,
                                                const char** refusal = nullptr) noexcept;

} // namespace sunrise::state
