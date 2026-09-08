#pragma once
#include <optional>

#include "definition.h"
namespace sunrise::state::build_data::combat_labels {
/** Resolves wire label hashes through the installed catalog. Unknown hashes refuse the mask. */
bool mask_for_labels(std::span<const std::uint32_t> labels,
                     std::array<std::byte, 40>& output) noexcept;
/** Validates before atomic publication; failure preserves the previous catalog. */
bool replace(const Catalog&) noexcept;
std::size_t count() noexcept;
/** Class facts only. Does not establish a kill, local ownership, activity or element. */
Source classify(std::span<const std::byte, 40> source,
                std::span<const std::byte, 40> actor) noexcept;
/** Selected authored forsaken requires a valid NPC mask and exclusive modifier range.
 * Absent catalog/marker returns unknown. Invalid marked masks must not enter race fallback.
 * Does not establish a kill or authorize overriding a contradictory native classification.
 */
VictimRace classify_victim(std::span<const std::byte, 40> victim) noexcept;
/** Unique authored base/boss/elite/megaboss/miniboss hash from a valid NPC victim mask.
 * Missing, conflicting, player or structurally unrecognized rank evidence remains absent.
 * This is rank identity, not a powerful predicate or contribution weight.
 */
std::optional<std::uint32_t> classify_victim_rank(std::span<const std::byte, 40> victim) noexcept;
} // namespace sunrise::state::build_data::combat_labels
