#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Booleans are stored as one byte that must read zero or one. */
[[nodiscard]] bool decode_flag(std::uint8_t stored, bool& value) noexcept {
    value = stored != 0;
    return stored <= 1;
}

/** An unbound reference stores the source a default instruction encodes to. */
[[nodiscard]] bool unused_source(const RewardInstructionRecord& record) noexcept {
    return record.opcode == 0 && record.bank == 0 && record.reserved == 0 && record.operand == 0;
}

void encode_value(const pursuits::ValueReference& value, PursuitValueRecord& record) noexcept {
    record.index = value.index;
    record.bound = static_cast<std::uint8_t>(value.bound);
    if (value.bound) {
        static_cast<void>(encode(value.source, record.source));
    }
}

[[nodiscard]] bool decode_value(const PursuitValueRecord& record,
                                pursuits::ValueReference& value) noexcept {
    value.index = record.index;
    if (!decode_flag(record.bound, value.bound)) {
        return false;
    }
    return value.bound ? decode(record.source, value.source) : unused_source(record.source);
}

[[nodiscard]] bool decode_operation(const PursuitOperationRecord& record,
                                    pursuits::OperationReference& value) noexcept {
    if (record.availability
        > static_cast<std::uint8_t>(pursuits::OperationAvailability::invalidReference)) {
        return false;
    }
    value.index = record.index;
    value.availability = static_cast<pursuits::OperationAvailability>(record.availability);
    return true;
}

} // namespace

/** Encodes one objective row and the bank ranges of its programs. */
bool encode(const objectives::Row& value, ObjectiveRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.completionValue = value.completionValue;
    record.definitionIndex = value.definitionIndex;
    record.itemValueIndex = value.itemValueIndex;
    record.optional = static_cast<std::uint8_t>(value.optional);
    record.countsDown = static_cast<std::uint8_t>(value.countsDown);
    record.adjustmentReadable = static_cast<std::uint8_t>(value.adjustmentReadable);
    record.itemProgress = static_cast<std::uint8_t>(value.itemProgress);
    record.itemValueMode = static_cast<std::uint8_t>(value.itemValueMode);
    record.readable = static_cast<std::uint8_t>(value.readable);
    record.targetAdjustment = {value.targetAdjustment.first, value.targetAdjustment.count};
    record.expression = {value.expression.first, value.expression.count};
    return true;
}

/** Decodes one objective row; the complete-domain validator checks its program ranges. */
bool decode(const ObjectiveRecord& record, objectives::Row& value) noexcept {
    value = {};
    if (record.itemValueMode > static_cast<std::uint8_t>(objectives::ItemValueMode::maximum)
        || !decode_flag(record.optional, value.optional)
        || !decode_flag(record.countsDown, value.countsDown)
        || !decode_flag(record.adjustmentReadable, value.adjustmentReadable)
        || !decode_flag(record.itemProgress, value.itemProgress)
        || !decode_flag(record.readable, value.readable)) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.completionValue = record.completionValue;
    value.definitionIndex = record.definitionIndex;
    value.itemValueIndex = record.itemValueIndex;
    value.itemValueMode = static_cast<objectives::ItemValueMode>(record.itemValueMode);
    value.targetAdjustment = {record.targetAdjustment.first, record.targetAdjustment.count};
    value.expression = {record.expression.first, record.expression.count};
    return true;
}

/** Encodes one completion contract with its declared objectives and references. */
bool encode(const pursuits::Definition& value, PursuitRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.lifetimeSeconds = value.lifetimeSeconds;
    record.definitionIndex = value.definitionIndex;
    record.readable = static_cast<std::uint8_t>(value.readable);
    record.suppressCompletion = static_cast<std::uint8_t>(value.suppressCompletion);
    record.requireAllObjectives = static_cast<std::uint8_t>(value.requireAllObjectives);
    record.requireNonOptionalCompletion =
        static_cast<std::uint8_t>(value.requireNonOptionalCompletion);
    record.automaticCompletion = static_cast<std::uint8_t>(value.automaticCompletion);
    record.objectiveCount = value.objectiveCount;
    record.objectives = value.objectives;
    encode_value(value.completionValue, record.completionValue);
    encode_value(value.completionContextValue, record.completionContextValue);
    record.secondaryOperation = {value.secondaryOperation.index,
                                 static_cast<std::uint8_t>(value.secondaryOperation.availability)};
    record.primaryOperation = {value.primaryOperation.index,
                               static_cast<std::uint8_t>(value.primaryOperation.availability)};
    return true;
}

/** Decodes one contract; the catalog validator checks its objective and item references. */
bool decode(const PursuitRecord& record, pursuits::Definition& value) noexcept {
    value = {};
    if (record.objectiveCount > record.objectives.size()
        || !decode_flag(record.readable, value.readable)
        || !decode_flag(record.suppressCompletion, value.suppressCompletion)
        || !decode_flag(record.requireAllObjectives, value.requireAllObjectives)
        || !decode_flag(record.requireNonOptionalCompletion, value.requireNonOptionalCompletion)
        || !decode_flag(record.automaticCompletion, value.automaticCompletion)
        || !decode_value(record.completionValue, value.completionValue)
        || !decode_value(record.completionContextValue, value.completionContextValue)
        || !decode_operation(record.secondaryOperation, value.secondaryOperation)
        || !decode_operation(record.primaryOperation, value.primaryOperation)) {
        return false;
    }
    const auto unused = std::span(record.objectives).subspan(record.objectiveCount);
    if (!std::all_of(
            unused.begin(), unused.end(), [](std::uint16_t index) { return index == 0; })) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.lifetimeSeconds = record.lifetimeSeconds;
    value.definitionIndex = record.definitionIndex;
    value.objectiveCount = record.objectiveCount;
    value.objectives = record.objectives;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
