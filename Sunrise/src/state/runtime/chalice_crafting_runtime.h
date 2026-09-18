#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state {
struct AccountState;
struct PendingSocketPlug;
} // namespace sunrise::state

namespace sunrise::state::runtime::detail::chalice {

inline constexpr std::uint32_t kChaliceHash = 1115550924U;
inline constexpr std::uint16_t kRuneValueBase = 2371;
inline constexpr std::uint16_t kUpgradeFlagBase = 5427;

/** Bounded native banks guarded together with the prepared socket after-image. */
struct State {
    std::array<std::int32_t, 12> runes{};
    std::array<std::uint8_t, 13> upgrades{};
    std::int32_t questProgress{};
    bool operator==(const State&) const = default;
};

/** Validate installed source slots against independent account-bank mapping tables. */
[[nodiscard]] bool configure_banks(std::span<const std::byte> flagSlots,
                                   std::span<const std::byte> valueSlots,
                                   std::span<const std::byte> flagMaps,
                                   std::span<const std::byte> valueMaps) noexcept;
[[nodiscard]] bool configure_sockets(std::span<const std::byte> socketTypes) noexcept;
[[nodiscard]] bool needs_item(std::uint16_t index) noexcept;
/** Retains only bounded predicates and identities, never a native item blob. */
[[nodiscard]] bool
configure_item(std::uint16_t index, std::uint32_t hash, std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool metadata_ready() noexcept;

/** The caller owns the encompassing SQLite transaction. */
[[nodiscard]] bool write(const State& before, const State& after) noexcept;
/** Overlay the prepared native banks on an account object before it is sent or committed. */
[[nodiscard]] bool project(const State& before,
                           const State& after,
                           std::span<std::uint8_t> flags,
                           std::span<std::int32_t> values) noexcept;
[[nodiscard]] bool stage(const AccountState& snapshot,
                         std::size_t characterIndex,
                         std::uint64_t targetInstanceSoid,
                         std::uint8_t socketLane,
                         std::uint16_t plugDefinitionIndex,
                         PendingSocketPlug& mutation) noexcept;

} // namespace sunrise::state::runtime::detail::chalice
