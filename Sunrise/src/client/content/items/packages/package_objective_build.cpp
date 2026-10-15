#include "package_objective_build.h"

#include <cstdint>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_expression.h"
#include "package_pursuit_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace objectives = state::build_data::objectives;

/** Completion control bytes of one objective row. */
constexpr std::size_t kOptionalOffset = 0x28;
constexpr std::size_t kAllowNegativeValueOffset = 0x2A;
constexpr std::size_t kAllowValueChangeWhenCompletedOffset = 0x2B;
constexpr std::size_t kCountsDownOffset = 0x2C;
/** Expression whose value the native evaluator subtracts from the completion value. */
constexpr std::size_t kTargetAdjustmentField = 0x38;

/** Counts the objective log line reports once the table is published. */
struct ObjectiveReport {
    std::size_t unreadable{};
    std::size_t unreadableAdjustments{};
    std::uint16_t firstUnreadableAdjustment{tables::items::kAbsentReference};
    std::size_t itemProgress{};
    std::size_t externalInputs{};
};

/** A refusal the installed data decides is final; a failed allocation is retried. */
enum class RowRead : std::uint8_t { read, unsupported, retry };

/** Reads one control byte, refusing any value other than zero or one. */
[[nodiscard]] bool
read_control(std::span<const std::byte> blob, std::size_t at, bool& value) noexcept {
    std::uint8_t raw = 0;
    if (!tables::read(blob, at, raw) || raw > 1) {
        return false;
    }
    value = raw != 0;
    return true;
}

/** Reads the completion controls of one bounded objective row. */
[[nodiscard]] bool read_completion_controls(std::span<const std::byte> blob,
                                            std::size_t at,
                                            objectives::Definition& definition) noexcept {
    return read_control(blob, at + kOptionalOffset, definition.optional)
           && read_control(blob, at + kAllowNegativeValueOffset, definition.allowNegativeValue)
           && read_control(blob,
                           at + kAllowValueChangeWhenCompletedOffset,
                           definition.allowValueChangeWhenCompleted)
           && read_control(blob, at + kCountsDownOffset, definition.countsDown);
}

/** A full instruction bank is decided by the data; failed storage is not. */
[[nodiscard]] RowRead bank_refusal(BankFailure failure) noexcept {
    return failure == BankFailure::full ? RowRead::unsupported : RowRead::retry;
}

/**
 * Reads one objective row and binds its progress and target-adjustment programs.
 * An unbindable program leaves the row unreadable rather than refusing the table.
 */
[[nodiscard]] RowRead read_objective(const RewardConditions& conditions,
                                     std::span<const std::byte> blob,
                                     std::size_t at,
                                     objectives::Definition& definition,
                                     ObjectiveReport& report) noexcept {
    if (!tables::read(blob, at, definition.definitionHash)
        || !tables::read(
            blob, at + tables::kObjectiveCompletionValueOffset, definition.completionValue)
        || !read_completion_controls(blob, at, definition)) {
        return RowRead::unsupported;
    }
    state::build_data::rewards::Range expression{};
    BankFailure failure{};
    definition.adjustmentReadable = conditions.read(
        blob, at + kTargetAdjustmentField, definition.targetAdjustment, expression, failure);
    if (failure != BankFailure::none) {
        return bank_refusal(failure);
    }
    if (!definition.adjustmentReadable) {
        definition.targetAdjustment.clear();
        if (report.unreadableAdjustments++ == 0) {
            report.firstUnreadableAdjustment = definition.definitionIndex;
        }
    }
    definition.readable = conditions.read(blob,
                                          at + tables::kObjectiveSourceExpressionField,
                                          definition.expression,
                                          expression,
                                          failure)
                          && !definition.expression.empty();
    if (failure != BankFailure::none) {
        return bank_refusal(failure);
    }
    if (!definition.readable) {
        definition.expression.clear();
        ++report.unreadable;
        return RowRead::read;
    }
    if (definition.expression.size() == 1) {
        const state::unlocks::Instruction& input = definition.expression.front();
        definition.itemProgress = input.opcode == state::unlocks::Opcode::loadValue
                                  && input.bank == state::unlocks::Bank::external
                                  && conditions.item_value(input.operand,
                                                           definition.itemValueIndex,
                                                           definition.itemValueMode);
    }
    if (definition.itemProgress) {
        ++report.itemProgress;
    } else if (state::unlocks::reads_external(definition.expression)
               || state::unlocks::reads_external(definition.targetAdjustment)) {
        ++report.externalInputs;
    }
    return RowRead::read;
}

} // namespace

bool build_objectives(const reader::Source& source,
                      reader::Scratch& scratch,
                      std::span<const std::byte> root,
                      const SlotMaps& maps) noexcept {
    if (state::build_data::objective_definitions_ready()) {
        return true;
    }
    std::uint32_t tag = 0;
    if (!tables::slot_tag(root, tables::kObjectiveTableSlot, tag)
        || tables::package_of(tag) == tables::kAbsentPackageId) {
        settle_pursuits_unsupported("objective_slot");
        return false;
    }
    try {
        std::vector<std::byte> blob;
        tables::Array rows{};
        if (!reader::read_tag(source, scratch, tag, blob)) {
            return false;
        }
        if (!tables::find_array_at(blob, tables::kTableArrayDescriptor, rows)
            || rows.elementClass != tables::kObjectiveRowClass || rows.count == 0
            || rows.count > objectives::kDefinitionCapacity || rows.dataOffset > blob.size()
            || rows.count > (blob.size() - rows.dataOffset) / tables::kObjectiveRowStride) {
            settle_pursuits_unsupported("objective_schema");
            return false;
        }
        RewardConditions conditions;
        if (!load_pursuit_conditions(conditions, source, scratch, root, maps)) {
            return false;
        }
        std::vector<objectives::Definition> definitions(static_cast<std::size_t>(rows.count));
        ObjectiveReport report{};
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            definitions[i].definitionIndex = static_cast<std::uint16_t>(i);
            const RowRead row = read_objective(conditions,
                                               blob,
                                               rows.dataOffset + i * tables::kObjectiveRowStride,
                                               definitions[i],
                                               report);
            if (row == RowRead::retry) {
                return false;
            }
            if (row == RowRead::unsupported) {
                core::log::writef(core::log::Channel::client,
                                  core::log::Level::warn,
                                  "ev=pkg stage=objectives result=fail row=%zu",
                                  i);
                settle_pursuits_unsupported("objective_row");
                return false;
            }
        }
        if (!state::build_data::publish_objective_definitions(definitions)) {
            settle_pursuits_unsupported("objective_publish");
            return false;
        }
        const bool complete = report.unreadable == 0 && report.unreadableAdjustments == 0;
        core::log::writef(core::log::Channel::client,
                          complete ? core::log::Level::info : core::log::Level::warn,
                          "ev=pkg stage=objectives result=ready rows=%zu unreadable=%zu "
                          "unreadable_adjustments=%zu first_unreadable_adjustment=%u "
                          "item_progress=%zu external_inputs=%zu",
                          definitions.size(),
                          report.unreadable,
                          report.unreadableAdjustments,
                          static_cast<unsigned>(report.firstUnreadableAdjustment),
                          report.itemProgress,
                          report.externalInputs);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::items::packages
