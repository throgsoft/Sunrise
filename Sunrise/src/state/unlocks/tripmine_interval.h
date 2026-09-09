#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include "../build_data/records/record_catalog.h"
#include "definition.h"

namespace sunrise::state::unlocks::records::tripmine {
// Semantic gameplay boundary; every bank index and milestone comes from the installed catalog.
inline constexpr std::uint32_t kRecordHash = 0x2FC2310AU;

struct Mapping {
    std::uint16_t progressValueIndex{};
    std::array<build_data::records::Interval,
               build_data::records::kIntervalPerRecordCapacity> intervals{};
};

/** Resolves one cumulative lane and the objective-derived thresholds extracted into intervals. */
[[nodiscard]] inline bool resolve(const build_data::records::Definition& record,
                                   Mapping& mapping) noexcept {
    namespace catalog = build_data::records;
    mapping = {};
    if (record.definitionHash != kRecordHash
        || record.completionFlagIndex >= kAccountFlagCapacity
        || record.redeemedCountValueIndex >= kObjectiveValueCapacity
        || record.intervalCount == 0 || record.intervalCount > mapping.intervals.size()) {
        return false;
    }
    mapping.progressValueIndex = record.objectiveValueIndex;
    for (std::size_t index = 0; index < record.objectiveCount; ++index) {
        catalog::Objective objective{};
        if (!catalog::objective(record, index, objective)) {
            return false;
        }
        const auto source = objective.sourceValueIndex != catalog::kUnavailableValueIndex
                                ? objective.sourceValueIndex : objective.valueIndex;
        if (index == 0) {
            mapping.progressValueIndex = source;
        } else if (source != mapping.progressValueIndex) {
            return false;
        }
    }
    if (mapping.progressValueIndex >= kObjectiveValueCapacity
        || mapping.progressValueIndex == record.redeemedCountValueIndex) {
        return false;
    }
    std::int32_t previous = 0;
    for (std::size_t index = 0; index < record.intervalCount; ++index) {
        auto& step = mapping.intervals[index];
        if (!catalog::interval(record, index, step) || step.completionValue <= previous
            || step.score > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        previous = step.completionValue;
    }
    return true;
}
} // namespace sunrise::state::unlocks::records::tripmine
