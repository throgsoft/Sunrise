#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../unlocks/unlocks_expression.h"
#include "../items/details/definition.h"

namespace sunrise::state::build_data::pursuits {

/** Native item, value and operation references use all bits set for an absent row. */
inline constexpr std::uint16_t kAbsent = items::details::kUnavailableItemIndex;
/** The installed pursuit objective block supports seven displayed objectives. */
inline constexpr std::size_t kObjectiveCapacity = 7;
/** The serialized reward-preview block has six fixed slots. */
inline constexpr std::size_t kRewardCapacity = 6;

/** A native value reference and its independently resolved saved or computed source. */
struct ValueReference {
    std::uint16_t index{kAbsent};
    bool bound{};
    unlocks::Instruction source{};
};

/** Missing data is distinct from an authored empty operation reference. */
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

/** A displayed reward is evidence about the preview, never an executable grant. */
struct RewardPreview {
    std::uint16_t itemIndex{kAbsent};
    std::int32_t quantity{};
    std::uint16_t conditionIndex{kAbsent};
    bool conditionReadable{};
    std::vector<unlocks::Instruction> condition;
};

/** Installed completion contract for one item, including explicitly unresolved effect edges. */
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
    std::vector<std::uint16_t> objectives;
    /** Unless suppressed, absence removes the step; a present value is incremented by one. */
    ValueReference completionValue;
    ValueReference completionContextValue;
    /** Native completion invokes the secondary reference before the primary reference. */
    OperationReference secondaryOperation;
    OperationReference primaryOperation;
    bool rewardsReadable{};
    std::vector<RewardPreview> rewards;
};

} // namespace sunrise::state::build_data::pursuits
