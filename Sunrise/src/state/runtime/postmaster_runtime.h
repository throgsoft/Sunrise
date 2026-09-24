#pragma once

#include "../account/account_state.h"

namespace sunrise::state {

/** A whole-instance claim, staged until its response and Family4 update fit. */
struct PendingPostmasterClaim {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t instanceSoid{};
    std::size_t characterIndex{};
    std::size_t inventoryIndex{};
    std::uint32_t expectedNextInventorySerial{};
    std::int32_t quantity{};
    std::uint16_t vendorIndex{};
    std::uint16_t definitionIndex{};
    std::uint16_t beforeInventoryRow{};
    std::uint16_t afterInventoryRow{};
    std::uint8_t sourceBucket{};
    bool prepared{};
};

/** Validates the installed Postmaster rule and a selected-character resident; quantity must be 1.
 */
[[nodiscard]] bool prepare_postmaster_claim(std::uint16_t vendorIndex,
                                            std::uint8_t sourceBucket,
                                            std::uint64_t instanceSoid,
                                            std::uint16_t definitionIndex,
                                            std::int32_t quantity,
                                            PendingPostmasterClaim& mutation) noexcept;
/** Rebuilds the after-image against current State without writing it. */
[[nodiscard]] bool preview_postmaster_claim(const PendingPostmasterClaim& mutation,
                                            AccountState& after) noexcept;
/** Rechecks the exact character/serial and installed rule under the SQLite lock; consumes mutation.
 */
[[nodiscard]] bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept;

} // namespace sunrise::state

namespace sunrise::state::runtime::detail {
/**
 * Applies installed FIFO policy, or redirects eligible equipment when its authored bucket is full.
 * Eviction updates the supplied character copy before the caller appends the incoming item.
 * @param before Account the capacity decision reads.
 * @param characterIndex Character receiving the reward.
 * @param item Reward whose placement is decided.
 * @param after Character copy receiving any eviction.
 * @param evictedInstanceSoid Receives the removed QueueZ resident, or zero for a stack.
 */
[[nodiscard]] bool place_instanced_reward(const AccountState& before,
                                          std::size_t characterIndex,
                                          account::inventory::Item& item,
                                          CharacterState& after,
                                          std::uint64_t& evictedInstanceSoid) noexcept;
} // namespace sunrise::state::runtime::detail
