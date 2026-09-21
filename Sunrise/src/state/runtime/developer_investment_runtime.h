#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state { struct PendingRecordRewardGrant; }

namespace sunrise::state::developer {

struct Result {
    bool accepted{};
    std::size_t changed{};
    const char* reason{"unavailable"};
};

inline constexpr std::size_t kItemGrantCopyLimit = 9;
struct ItemGrantTarget {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t signInSeconds{};
};
/** Read-only binding captured before a developer request enters the BAP queue. */
[[nodiscard]] bool read_item_grant_target(ItemGrantTarget& target) noexcept;
/** Stages one bounded batch without committing. The BAP owner must encode its normal reward
 * notification before commit_record_reward, under one enclosing store transaction.
 * accepted with !pending.prepared means an already-held pursuit was left untouched.
 */
[[nodiscard]] Result prepare_item_grant(std::uint16_t index, std::int32_t quantity,
                                        std::uint32_t expectedHash, ItemGrantTarget target,
                                        PendingRecordRewardGrant& pending) noexcept;

/** Grants through the current reward/acquisition policy; one request is atomic.
 * expectedHash zero accepts any installed identity. Existing pursuits remain untouched.
 */
[[nodiscard]] Result
grant_item(std::uint16_t index, std::int32_t quantity = 1, std::uint32_t expectedHash = 0) noexcept;
/** Developer rune-counter grant: rune 0..11, or 12 for all; no slot/upgrade unlocks. */
[[nodiscard]] Result grant_chalice_runes(std::uint8_t rune, std::int32_t quantity) noexcept;
/** Edits held objective progress only. Lane zero means all declared objectives, never expiry. */
[[nodiscard]] Result
set_quest(std::uint16_t index, std::int32_t value, std::uint8_t lane = 0) noexcept;
/** Edits one objective of one held bounty, preserving other copies and expiry. */
[[nodiscard]] Result set_bounty_lane(std::uint64_t instanceSoid, std::uint16_t index,
                                     std::int32_t value, std::uint8_t lane) noexcept;
/** Completes the selected character's held, resolved, unexpired pursuits atomically.
 * Does not acquire items, redeem rewards, or publish gameplay events.
 */
[[nodiscard]] Result complete_pursuits() noexcept;
/** Same objective completion policy, limited to held expiring bounties; preserves quests. */
[[nodiscard]] Result complete_bounties() noexcept;
/** Grants/reuses and completes one installed bounty atomically for a developer page.
 * Preserves
 * expiry, unrelated pursuits and rewards; rolls back acquisition if completion fails.
 */
[[nodiscard]] Result grant_complete_bounty(std::uint16_t index,
                                           std::uint32_t expectedHash) noexcept;

/** Removes all unequipped resident copies of an installed item; no rewards, claims or refunds. */
[[nodiscard]] Result drop_item(std::uint16_t index) noexcept;
/** Removes only objective-bearing held pursuits; keeps the oven, gear and all saved reward banks.
 */
[[nodiscard]] Result drop_pursuits() noexcept;
/** Removes held bounties classified by installed metadata; preserves quests, stacks and rewards. */
[[nodiscard]] Result drop_bounties() noexcept;
/** Removes the selected character's held Engrams bucket residents without decrypting them. */
[[nodiscard]] Result drop_engrams() noexcept;
/** Removes only the selected character's unequipped kinetic, energy and heavy weapons. */
[[nodiscard]] Result drop_weapons() noexcept;
/** Removes only the selected character's unequipped helmet, gauntlets, chest, legs and class armor. */
[[nodiscard]] Result drop_armor() noexcept;
/** Clears the installed pass's mapped reward claims (all classes) and all lanes of account
 * progressions 40/41 in one SQLite transaction. Zero XP maps to native rank 1.
 * Keeps granted items, other ownership, pursuits and artifact state. Incomplete metadata or
 * failed persistence refuses the whole reset. changed counts unique flags and nonzero lanes.
 */
[[nodiscard]] Result drop_season_pass() noexcept;
// Replication belongs to callers; queued preparation borrows the enclosing BAP transaction.
} // namespace sunrise::state::developer
