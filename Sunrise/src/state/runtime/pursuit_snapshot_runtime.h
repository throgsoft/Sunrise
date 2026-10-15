#pragma once

#include <cstdint>
#include <vector>

#include "../account/account_state.h"
#include "../build_data/pursuits/definition.h"
#include "pursuit_completion_runtime.h"

namespace sunrise::state {

/** Why no snapshot was produced; a partial snapshot is never returned. */
enum class PursuitSnapshotRefusal : std::uint8_t {
    none,
    /** The objective or pursuit catalog is not published, or was republished mid-read. */
    catalogUnavailable,
    /** No valid saved account. */
    accountUnavailable,
    /** The requested character is absent, or no character is selected for zero. */
    characterUnavailable,
    /** A SQLite or allocation failure. */
    storage,
};

/** A displayed reward preview is evidence, not a grant; only its condition is evaluated. */
enum class PursuitConditionResult : std::uint8_t { unconditional, met, unmet, unknown };

struct PursuitRewardStatus {
    std::uint16_t itemIndex{build_data::pursuits::kAbsent};
    std::int32_t quantity{};
    PursuitConditionResult condition{PursuitConditionResult::unknown};
};

/** One unequipped pursuit-bucket resident and its saved-bank evaluation. */
struct HeldPursuitStatus {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{build_data::pursuits::kAbsent};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint32_t flags{};
    /** Positive marks a bounty; its saved deadline is not available on this server. */
    std::int32_t lifetimeSeconds{};
    bool contractFound{};
    PursuitCompletionStatus completion{};
    bool rewardsReadable{};
    std::vector<PursuitRewardStatus> rewards;
};

/** One coherent saved view of a character's held pursuits. */
struct PursuitSnapshot {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    CharacterClass characterClass{};
    std::uint64_t investmentRevision{};
    std::uint64_t definitionsRevision{};
    std::vector<HeldPursuitStatus> held;
};

/**
 * Reads saved inventory and unlock banks under one store lock, then evaluates installed
 * contracts against those copies. Never writes, never draws, never publishes.
 * @param characterSoid A character of the saved account, or zero for the selected character.
 * @param output Receives the snapshot; cleared on any refusal.
 * @return None on success, otherwise why no snapshot was produced.
 */
[[nodiscard]] PursuitSnapshotRefusal read_pursuit_snapshot(std::uint64_t characterSoid,
                                                           PursuitSnapshot& output) noexcept;

} // namespace sunrise::state
