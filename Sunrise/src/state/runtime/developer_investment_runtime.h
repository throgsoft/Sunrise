#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::developer {

struct Result {
    bool accepted{};
    std::size_t changed{};
    const char* reason{"unavailable"};
};

/** Grants through the current reward/acquisition policy; one request is atomic.
 * expectedHash zero accepts any installed identity. Existing pursuits remain untouched.
 */
[[nodiscard]] Result
grant_item(std::uint16_t index, std::int32_t quantity = 1, std::uint32_t expectedHash = 0) noexcept;
/** Edits held objective progress only. Lane zero means all declared objectives, never expiry. */
[[nodiscard]] Result
set_quest(std::uint16_t index, std::int32_t value, std::uint8_t lane = 0) noexcept;
/** Completes the selected character's held, resolved, unexpired pursuits atomically.
 * Does not acquire items, redeem rewards, or publish gameplay events.
 */
[[nodiscard]] Result complete_pursuits() noexcept;
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
// Every function releases its SQLite transaction before returning. Replication belongs to callers.
} // namespace sunrise::state::developer
