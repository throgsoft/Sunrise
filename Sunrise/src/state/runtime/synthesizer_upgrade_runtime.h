#pragma once

#include "../account/account_state.h"

namespace sunrise::state::runtime::detail::synthesizer {

/** Replaces the uniquely held Synthesizer by its next tier in a private redemption view.
 * The caller owns source consumption, outbound validation and the SQLite transaction.
 * Native defaults and newly opened lanes come from installed item details. */
[[nodiscard]] bool stage_upgrade(CharacterState& character) noexcept;

} // namespace sunrise::state::runtime::detail::synthesizer
