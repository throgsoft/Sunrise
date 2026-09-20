#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../build_data/crafting/definition.h"
#include "../investment/investment.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail::synthesizer {

/** Installs decoded socket costs for the three crafting tiers and recycling lane. */
void configure_socket_costs(const build_data::crafting::SynthesizerCosts& costs) noexcept;

/** Installs all twelve validated native output flag pairs together. */
[[nodiscard]] bool
configure_mote_output_flags(std::span<const build_data::crafting::MoteOutput> outputs) noexcept;
[[nodiscard]] bool mote_output_flags_ready() noexcept;

/** Derives only the native Mote output flags from current profile ownership. These kind-0
 * slots have no Family-4 bank mapping, so the caller publishes this transient Family-5
 * full replacement. previousMoteMask is cumulative publication history, not last ownership:
 * every previously published Mote needs an explicit clear on every later replacement.
 * Flags use native tri-state values: 1 clears, 2 sets; 0 is not an explicit clear.
 * Never persist it as unlock progress. Returns false without changing the
 * family if metadata, inventory or override capacity cannot represent the whole result. */
[[nodiscard]] bool project_mote_output_flags(const AccountState& account,
                                             Family5State& family,
                                             std::uint16_t previousMoteMask = 0) noexcept;

[[nodiscard]] std::uint16_t
mote_ownership_mask(std::span<const account::inventory::ProfileItem> rows) noexcept;

/** Retain clears across full replacements. Advance this history only after publication;
 * use mote_ownership_mask, not this history, to decide whether inventory changed. */
[[nodiscard]] std::uint16_t
mote_publication_mask(std::span<const account::inventory::ProfileItem> rows,
                      std::uint16_t previousMoteMask) noexcept;

/** Detects a change to the owned Mote set, ignoring compaction, serials and seen bits. */
[[nodiscard]] bool
mote_ownership_changed(std::span<const account::inventory::ProfileItem> before,
                       std::span<const account::inventory::ProfileItem> after) noexcept;

[[nodiscard]] bool is_container(std::uint32_t definitionHash) noexcept;
[[nodiscard]] bool is_mote(std::uint32_t definitionHash) noexcept;

/** Stages synthesis or reversible recycling and the action-socket reset in PendingSocketPlug.
 * The existing socket commit owns the exact before-image check and SQLite transaction.
 * Sunrise recycling refunds the paired native synthesis cost; retail payout is not decoded. */
[[nodiscard]] bool stage_exchange(const AccountState& snapshot,
                                  std::size_t characterIndex,
                                  std::uint64_t targetInstanceSoid,
                                  std::uint8_t socketLane,
                                  std::uint16_t plugDefinitionIndex,
                                  PendingSocketPlug& mutation) noexcept;

/** Replaces the uniquely held Synthesizer by its next tier in the caller's redemption view. */
[[nodiscard]] bool stage_upgrade(CharacterState& character) noexcept;

} // namespace sunrise::state::runtime::detail::synthesizer
