#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace sunrise::state::bounties::areas {

/** A bubble name hash is not globally unique (Mars Hellas View / Moon Tower of Woe).
 * Only an authenticated generated-world binding may produce this normalized fact.
 */
struct BubbleFact final {
    std::uint32_t scenarioTag{};
    std::uint32_t bubbleHash{};
    friend constexpr bool operator==(const BubbleFact&, const BubbleFact&) = default;
};

/** One lane's area alternatives, ANDed with all ordinary kill predicates. */
struct Requirement final {
    std::uint32_t scenarioTag{};
    std::array<std::uint32_t, 3> bubbleHashes{};
};

[[nodiscard]] constexpr bool matches(const Requirement& requirement,
                                     const std::optional<BubbleFact>& fact) noexcept {
    if (!fact || requirement.scenarioTag == 0 || fact->scenarioTag != requirement.scenarioTag
        || fact->bubbleHash == 0)
        return false;
    for (const auto hash : requirement.bubbleHashes)
        if (hash != 0 && hash == fact->bubbleHash) return true;
    return false;
}

} // namespace sunrise::state::bounties::areas
