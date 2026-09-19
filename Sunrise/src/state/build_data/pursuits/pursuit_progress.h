#pragma once

#include <cstdint>
#include <span>

namespace sunrise::state::build_data::pursuits {

/** What one held pursuit's progress amounts to, measured against what it declares. */
struct Progress {
    /** Objectives the definition declares. Zero for every item that is not a pursuit. */
    std::uint8_t objectiveCount{};
    /** Item-backed objectives whose lane has reached the declared completion value. */
    std::uint8_t completeCount{};
    /** True when every declared objective resolved to a definition in the objective table. */
    bool resolved{};
    /** True when every declared objective is completed by the item's own lane, so writing the
     * lanes can finish the pursuit. A shared or unsupported source leaves this clear. */
    bool itemBacked{};
};

/**
 * Measures one held pursuit against the objectives its definition declares.
 *
 * The client renders its own bars and will send a redemption for anything it believes is finished,
 * so the payout is judged here instead, against installed content rather than account state. An
 * objective that does not resolve leaves `resolved` clear rather than counting as complete, so a
 * definition this build cannot read never pays out. A shared or unsupported source is never
 * completed by the item's own lane and needs its own evaluator.
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
