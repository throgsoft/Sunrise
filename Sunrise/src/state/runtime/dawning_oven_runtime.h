#pragma once

#include "../account/inventory/dawning_oven_state.h"
#include "../unlocks/definition.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail::dawning {

using State = account::inventory::dawning::State;

/** Reads SQLite material balances and recipe flags, preserving previously saved progress. */
[[nodiscard]] bool read(State& output) noexcept;
/** Compare-and-write the bounded banks; the caller owns the encompassing SQLite transaction. */
[[nodiscard]] bool write(const State& before, const State& after) noexcept;

/** Oven actions consume material counters and profile stacks as one prepared socket mutation. */
[[nodiscard]] bool stage(const AccountState& snapshot,
                         std::size_t characterIndex,
                         std::uint64_t targetInstanceSoid,
                         std::uint8_t socketLane,
                         std::uint16_t plugDefinitionIndex,
                         PendingSocketPlug& mutation) noexcept;

/** Counter rewards are virtual materials: neither ingredient plugs nor pickups are residents. */
[[nodiscard]] bool credit(State& state,
                          std::uint32_t definitionHash,
                          std::int32_t quantity,
                          std::int32_t& credited) noexcept;
[[nodiscard]] bool ingredient_installed(std::size_t index) noexcept;

} // namespace sunrise::state::runtime::detail::dawning
