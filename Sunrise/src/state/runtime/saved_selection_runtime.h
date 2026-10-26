#pragma once

#include <cstddef>
#include <cstdint>

#include "../account/account_state.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/definition.h"

namespace sunrise::state {

/** How far one read of the selected character's saved view got. */
enum class SavedSelectionRead : std::uint8_t {
    read,
    /** The account rows could not be read. */
    accountStorage,
    /** The account rows name no usable account. */
    accountInvalid,
    /** No character is selected, so no banks were read. */
    unselected,
    /** The selected character's unlock banks could not be read. */
    bankStorage,
};

/**
 * One account view, its selected character's slot and unlock banks, and the revision, copied
 * under one store lock. A whole account and a bank table are large, so callers own it on the heap.
 */
template <typename Account> struct SavedSelection {
    Account account{};
    unlocks::Table banks{};
    /** The selected character's slot, which also owns its unlock banks. */
    std::size_t slot{};
    /** Account change generation read under the same lock. */
    std::uint64_t revision{};

    /** @return The selected character's evaluation context; evaluation never draws. */
    [[nodiscard]] reward_resolution::Context context() const noexcept {
        return {banks, account.characters[slot].characterClass, 0, nullptr};
    }
};

/**
 * Copies the whole account, the selected character's banks and the revision under one store lock,
 * so inventory and banks come from the same saved state. Inside a caller's savepoint the copy
 * includes that savepoint's own writes.
 * @param output Receives the view; only fields read before a refusal are set.
 */
[[nodiscard]] SavedSelectionRead
read_saved_selection(SavedSelection<AccountState>& output) noexcept;

/**
 * Copies the account keys, the selected character's banks and the revision under one store lock,
 * for a caller that needs no inventory.
 * @param output Receives the view; only fields read before a refusal are set.
 */
[[nodiscard]] SavedSelectionRead
read_saved_selection(SavedSelection<AccountIdentity>& output) noexcept;

} // namespace sunrise::state
