#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::investment_edit {

struct Result {
    bool accepted{};
    std::size_t changed{};
    const char* reason{"unavailable"};
};

/** Grants one installed item through the record reward policy, which owns bucket placement,
 * Postmaster overflow and stacking. A Dawning ingredient stages its authoritative balance and
 * pickup queue row instead of a resident. Existing held pursuits are left untouched.
 * @param index Installed item to grant.
 * @param quantity Instanced copies, or the stack size for a stackable item.
 * @param expectedHash Installed identity the caller resolved, or zero to accept any.
 */
[[nodiscard]] Result
grant_item(std::uint16_t index, std::int32_t quantity, std::uint32_t expectedHash) noexcept;

/** Sets one objective lane of one held pursuit, preserving other copies and expiry.
 * Lane zero carries the expiry deadline rather than an objective and is never written.
 */
[[nodiscard]] Result set_objective_lane(std::uint64_t instanceSoid,
                                        std::uint16_t index,
                                        std::int32_t value,
                                        std::uint8_t lane) noexcept;
/** Completes the selected character's held, resolved, unexpired bounties; preserves quests. */
[[nodiscard]] Result complete_bounties() noexcept;

/** Removes held bounties classified by installed metadata; preserves quests, stacks and rewards. */
/**
 * Removes everything the selected character or the account holds in one installed bucket.
 * Equipped residents are never removed. The bucket is named by installed metadata, so no
 * category is enumerated here.
 * @param bucketId Installed inventory bucket to empty.
 * @return Rows removed, or a reason nothing was committed.
 */
[[nodiscard]] Result drop_bucket(std::uint8_t bucketId) noexcept;

/**
 * Sets the quantity of one held row, removing it when the quantity reaches zero.
 * @param instanceSoid Instanced resident, or zero for a stack or profile row.
 * @param definitionIndex Installed item the row holds.
 * @param quantity Quantity to store, bounded by the installed stack limit.
 * @return Rows changed, or a reason nothing was committed.
 */
[[nodiscard]] Result set_held_quantity(std::uint64_t instanceSoid,
                                       std::uint16_t definitionIndex,
                                       std::int32_t quantity) noexcept;

[[nodiscard]] Result drop_bounties() noexcept;
/** Removes the selected character's held Engrams bucket residents without decrypting them. */
[[nodiscard]] Result drop_engrams() noexcept;
/** Removes only the selected character's unequipped kinetic, energy and heavy weapons. */
[[nodiscard]] Result drop_weapons() noexcept;
/** Removes only the selected character's unequipped helmet, gauntlets, chest, legs and class armor.
 */
[[nodiscard]] Result drop_armor() noexcept;
/** Clears the installed pass's mapped reward claims (all classes) and all lanes of account
 * progressions 40/41 in one SQLite transaction. Zero XP maps to native rank 1.
 * Keeps granted items, other ownership, pursuits and artifact state. Incomplete metadata or
 * failed persistence refuses the whole reset. changed counts unique flags and nonzero lanes.
 */
[[nodiscard]] Result drop_season_pass() noexcept;
// Replication belongs to callers; every edit here commits its own store transaction.
} // namespace sunrise::state::investment_edit
