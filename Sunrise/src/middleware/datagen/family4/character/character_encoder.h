#pragma once

#include <span>

#include "../../../../state/account/account_state.h"
#include "../../../../state/equipment/light/definition.h"
#include "../loadout/definition.h"

namespace sunrise::middleware::datagen::family4::character {

/**
 * Encodes one selected-character object from live State and resolved installed mappings.
 * @param state Validated authored character identity and policy state.
 * @param resolvedLoadout Row-sorted inventory and equipment mappings for this character.
 * @param lightEvaluation Complete raw and aggregate equipment-light values.
 * @param output Exact runtime-mapped character-object storage.
 * @param requireCollectibleSpace
 * Grant preflight reserves every applicable synthetic prerequisite.
 * Normal publication defers
 * only synthetic rows that do not fit, preserving owned items on older saves.
 * @return True when
 * State, mappings, and the mapped object span fit the native layout.
 */
[[nodiscard]] bool encode(const state::CharacterState& state,
                          const loadout::ResolvedLoadout& resolvedLoadout,
                          const state::equipment::light::Evaluation& lightEvaluation,
                          std::span<std::byte> output,
                          bool requireCollectibleSpace = false) noexcept;

} // namespace sunrise::middleware::datagen::family4::character
