#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../build_data/crafting/definition.h"

namespace sunrise::state {
struct AccountState;
struct PendingSocketPlug;
} // namespace sunrise::state

namespace sunrise::state::runtime::detail::chalice {

using build_data::crafting::kChaliceHash;
using build_data::crafting::kRuneValueBase;
using build_data::crafting::kUpgradeFlagBase;

/** Bounded native banks guarded together with the prepared socket after-image. */
struct State {
    std::array<std::int32_t, 12> runes{};
    std::array<std::uint8_t, 13> upgrades{};
    std::int32_t questProgress{};
    bool operator==(const State&) const = default;
};

/** Installs one fully decoded and validated package snapshot for gameplay. */
[[nodiscard]] bool
configure_metadata(std::span<const build_data::crafting::ChalicePlug> plugs) noexcept;
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
