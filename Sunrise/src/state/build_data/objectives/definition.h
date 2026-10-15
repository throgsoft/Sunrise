#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../unlocks/unlocks_expression.h"

namespace sunrise::state::build_data::objectives {

/** The shipped build declares 8,483 objectives. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 16384;
/** All bits set mark an objective whose progress is not a per-item value. */
inline constexpr std::uint16_t kNoItemValue = 0xFFFF;

/** Whether an item value uses the inspected instance or the maximum over held instances. */
enum class ItemValueMode : std::uint8_t { context, maximum };

/** One row of the dense installed objective table. */
struct Definition {
    std::uint32_t definitionHash{};
    std::int32_t completionValue{};
    std::uint16_t definitionIndex{};
    bool optional{};
    bool allowNegativeValue{};
    bool allowValueChangeWhenCompleted{};
    bool countsDown{};
    /** False when the optional target adjustment could not be bound; empty is valid. */
    bool adjustmentReadable{};
    /** The native evaluator subtracts this expression's value from completionValue. */
    std::vector<unlocks::Instruction> targetAdjustment;
    /** The progress source is a per-item value that each instance carries. */
    bool itemProgress{};
    ItemValueMode itemValueMode{};
    /** Native value-table index of the per-item source, or kNoItemValue. */
    std::uint16_t itemValueIndex{kNoItemValue};
    /** False when the installed progress expression could not be bound. */
    bool readable{};
    std::vector<unlocks::Instruction> expression;
};

} // namespace sunrise::state::build_data::objectives
