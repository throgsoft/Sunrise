#pragma once
#include "../account/account_state.h"

namespace sunrise::state::runtime::detail::dawning {
/** Credits only the objectives whose event is this successful bake or cookie delivery.
 * All lane indices, caps and lifetimes come from installed definitions. */
[[nodiscard]] bool credit_bounties(CharacterState& character,
                                   bool delivery,
                                   std::uint32_t cookieHash,
                                   std::int64_t now) noexcept;
} // namespace sunrise::state::runtime::detail::dawning
