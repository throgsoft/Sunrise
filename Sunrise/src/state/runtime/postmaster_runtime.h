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

namespace runtime::detail {
/**
 * Only an actually full authored bucket may redirect an ordinary instanced reward.
 * Lost Items is installed kFifo, so a full one names its oldest resident for eviction rather
 * than refusing the arrival. Nothing is removed here; the caller owns the mutable character.
 * @param before Account the capacity decision reads.
 * @param characterIndex Character receiving the reward.
 * @param item Reward whose placement is decided.
 * @param evictedInstanceSoid Receives the resident the caller must drop, or zero.
 */
[[nodiscard]] bool place_instanced_reward(const AccountState& before,
                                          std::size_t characterIndex,
                                          account::inventory::Item& item,
                                          std::uint64_t& evictedInstanceSoid) noexcept;
} // namespace runtime::detail

} // namespace sunrise::state
