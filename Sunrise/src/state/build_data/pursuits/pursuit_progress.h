#pragma once

#include <cstdint>
#include <span>

namespace sunrise::state::build_data::pursuits {

/** What one held pursuit's progress amounts to, measured against what it declares. */
struct Progress {
    /** Objectives the definition declares. Zero for every item that is not a pursuit. */
    std::uint8_t objectiveCount{};
    /** Declared objectives whose lane has reached the value that objective completes at. */
    std::uint8_t completeCount{};
    /** True when every declared objective resolved to a definition in the objective table. */
    bool resolved{};
};

/**
 * Measures one held pursuit against the objectives its definition declares.
 *
 * The client renders a bounty's bars from the definition alone, and it will happily send a
 * redemption for one it believes is finished. Believing it is the difference between a server that
 * owns investment and one that mirrors a client: a redemption has to be judged against the item's
 * own objectives and the values they complete at, both of which are content, not account state.
 *
 * An objective that does not resolve leaves `resolved` clear rather than counting as complete, so
 * a definition this build cannot read never pays out.
 *
 * @param itemDefinitionIndex Native item-definition index of the held pursuit.
 * @param lanes Current value of each of the item instance's progress lanes.
 * @return The measurement, all zero when the item is not a pursuit.
 */
[[nodiscard]] Progress measure(std::uint16_t itemDefinitionIndex,
                               std::span<const std::int32_t> lanes) noexcept;

/**
 * @param itemDefinitionIndex Native item-definition index of the held pursuit.
 * @param lanes Current value of each of the item instance's progress lanes.
 * @return True when the item is a pursuit, resolved whole, and every objective is complete.
 */
[[nodiscard]] bool complete(std::uint16_t itemDefinitionIndex,
                            std::span<const std::int32_t> lanes) noexcept;

} // namespace sunrise::state::build_data::pursuits
