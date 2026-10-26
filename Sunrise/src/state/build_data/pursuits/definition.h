#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../unlocks/unlocks_expression.h"
#include "../items/details/definition.h"

namespace sunrise::state::build_data::pursuits {

/** Native item, value and operation references use all bits set for an absent row. */
inline constexpr std::uint16_t kAbsent = items::details::kUnavailableItemIndex;
/** The shipped build declares 1,941 pursuit contracts. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 4096;
/** The installed pursuit objective block supports seven displayed objectives. */
inline constexpr std::size_t kObjectiveCapacity = 7;

/** A native value reference and the saved or computed source it binds to, when bound. */
struct ValueReference {
    std::uint16_t index{kAbsent};
    bool bound{};
    unlocks::Instruction source{};
};

/**
 * Why an operation body is or is not available. Missing data is distinct from an authored absence.
 */
enum class OperationAvailability : std::uint8_t {
    absent,
    missingTable,
    unresolvedBody,
    invalidReference,
};

struct OperationReference {
    std::uint16_t index{kAbsent};
    OperationAvailability availability{OperationAvailability::absent};
};

/** The installed completion contract for one item. Unresolved effect edges are kept and marked. */
struct Definition {
    std::uint16_t definitionIndex{kAbsent};
    std::uint32_t definitionHash{};
    bool readable{};
    /** Positive for a bounty, which expires this many seconds after it is acquired. */
    std::int32_t lifetimeSeconds{};
    bool suppressCompletion{};
    bool requireAllObjectives{};
    bool requireNonOptionalCompletion{};
    bool automaticCompletion{};
    /** Objective-table indices. Slots past objectiveCount stay zero. */
    std::array<std::uint16_t, kObjectiveCapacity> objectives{};
    std::uint8_t objectiveCount{};
    /** Unless suppressed, an absent value removes the step and a present one is incremented. */
    ValueReference completionValue;
    ValueReference completionContextValue;
    /** Native completion invokes the secondary reference before the primary reference. */
    OperationReference secondaryOperation;
    OperationReference primaryOperation;
};

} // namespace sunrise::state::build_data::pursuits
