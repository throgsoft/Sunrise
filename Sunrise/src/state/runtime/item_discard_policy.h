#pragma once

#include <array>
#include <cstdint>

#include "bounty_reward_policy_data.h"

namespace sunrise::state::item_discard {

/** Local discard behavior for materials already supported by the investment defaults/rewards. */
enum class Mode : std::uint8_t { one, entireStack };
struct Policy {
    std::uint32_t hash;
    Mode mode;
};

// Gunsmith Materials are an existing investment-default wallet row. Enhancement Cores already
// have a semantic reward identity. These two modes preserve the source's supported behavior;
// installed indices, bucket placement and stack limits are deliberately not shipped here.
inline constexpr std::array<Policy, 2> kPolicies{{
    {runtime::detail::bounty_policy::kEnhancementCoreHash, Mode::one},
    {685157383U, Mode::entireStack},
}};

/** Unknown definitions have no inferred deletion policy. */
[[nodiscard]] constexpr const Policy* find(std::uint32_t hash) noexcept {
    for (const auto& policy : kPolicies)
        if (policy.hash == hash) return &policy;
    return nullptr;
}

/** Observed quantity is a precondition; the local mode chooses all versus one. */
[[nodiscard]] constexpr std::int32_t
quantity(const Policy& policy, std::int32_t held, std::int32_t maxStack) noexcept {
    return held > 0 && held <= maxStack ? (policy.mode == Mode::entireStack ? held : 1) : 0;
}

} // namespace sunrise::state::item_discard
