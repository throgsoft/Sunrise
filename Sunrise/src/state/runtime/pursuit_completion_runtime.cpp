#include "pursuit_completion_runtime.h"

#include <cstddef>
#include <span>

#include "../build_data/runtime.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/unlocks_expression.h"

namespace sunrise::state {
namespace {

namespace objectives = build_data::objectives;
namespace pursuits = build_data::pursuits;

/** The saved completion gate value. An absent reference reads as zero. */
struct CompletionValue {
    std::int32_t value{};
    bool known{true};
    /** The reference is bound outside the saved banks. */
    bool external{};
};

/** Names why an objective cannot be evaluated from the saved banks, before any bank is read. */
[[nodiscard]] PursuitObjectiveUnknown
unevaluable_objective(const objectives::Definition& objective) noexcept {
    if (!objective.readable) {
        return PursuitObjectiveUnknown::expressionUnreadable;
    }
    if (!objective.adjustmentReadable) {
        return PursuitObjectiveUnknown::adjustmentUnreadable;
    }
    if (objective.itemProgress) {
        return PursuitObjectiveUnknown::itemProgressNotSaved;
    }
    if (unlocks::reads_external(objective.expression)
        || unlocks::reads_external(objective.targetAdjustment)) {
        return PursuitObjectiveUnknown::externalValue;
    }
    return PursuitObjectiveUnknown::none;
}

/** Evaluates one objective's progress and adjusted target with its installed comparison. */
[[nodiscard]] PursuitObjectiveStatus
inspect_pursuit_objective(const objectives::Definition& objective,
                          const reward_resolution::Context& context) noexcept {
    PursuitObjectiveStatus status{};
    status.index = objective.definitionIndex;
    status.hash = objective.definitionHash;
    status.target = objective.completionValue;
    status.unknown = unevaluable_objective(objective);
    if (status.unknown != PursuitObjectiveUnknown::none) {
        return status;
    }
    std::int32_t adjustment = 0;
    if ((!objective.targetAdjustment.empty()
         && !reward_resolution::evaluate(objective.targetAdjustment, context, adjustment))
        || !reward_resolution::evaluate(objective.expression, context, status.value)) {
        status.unknown = PursuitObjectiveUnknown::sourceUnavailable;
        return status;
    }
    // The native signed subtraction wraps at the 32-bit boundary.
    status.target = static_cast<std::int32_t>(static_cast<std::uint32_t>(status.target)
                                              - static_cast<std::uint32_t>(adjustment));
    status.known = true;
    status.complete =
        objective.optional
        || (objective.countsDown ? status.value != 0 && status.value <= status.target
                                 : status.target != 0 && status.value >= status.target);
    return status;
}

/**
 * Evaluates every declared objective, then the aggregate under the contract's all-or-any mode.
 * An unknown objective still leaves the aggregate determinate when a known one settles it.
 */
void inspect_pursuit_objectives(const pursuits::Definition& definition,
                                const reward_resolution::Context& context,
                                PursuitCompletionStatus& status) noexcept {
    status.objectiveCount = static_cast<std::uint8_t>(definition.objectiveCount);
    std::size_t completed = 0;
    std::size_t incomplete = 0;
    std::size_t unknown = 0;
    for (std::size_t i = 0; i < definition.objectiveCount; ++i) {
        PursuitObjectiveStatus& result = status.objectives[i];
        result.index = definition.objectives[i];
        objectives::Definition objective{};
        if (!build_data::find_objective_definition(result.index, objective)) {
            result.unknown = PursuitObjectiveUnknown::definitionUnavailable;
            ++unknown;
            continue;
        }
        if (definition.requireNonOptionalCompletion && objective.optional) {
            result.hash = objective.definitionHash;
            result.known = true;
            result.skipped = true;
            result.target = objective.completionValue;
            continue;
        }
        result = inspect_pursuit_objective(objective, context);
        if (!result.known) {
            ++unknown;
        } else if (result.complete) {
            ++completed;
        } else {
            ++incomplete;
        }
    }
    status.objectivesKnown =
        unknown == 0 || (definition.requireAllObjectives ? incomplete != 0 : completed != 0);
    status.objectivesComplete =
        definition.requireAllObjectives
            ? unknown == 0 && incomplete == 0
                  && (!definition.requireNonOptionalCompletion || completed != 0)
            : completed != 0;
}

/** Reads the saved completion value through its bound load instruction. */
[[nodiscard]] CompletionValue
read_completion_value(const pursuits::ValueReference& reference,
                      const reward_resolution::Context& context) noexcept {
    CompletionValue result{};
    if (reference.index == pursuits::kAbsent) {
        return result;
    }
    result.external = reference.bound && reference.source.bank == unlocks::Bank::external;
    result.known = reference.bound && reference.source.opcode == unlocks::Opcode::loadValue
                   && reward_resolution::evaluate(
                       std::span{&reference.source, std::size_t{1}}, context, result.value);
    return result;
}

/** Only an authored absent reference needs no operation body. */
[[nodiscard]] bool
absent_pursuit_operation(const pursuits::OperationReference& operation) noexcept {
    return operation.index == pursuits::kAbsent
           && operation.availability == pursuits::OperationAvailability::absent;
}

/** True for an absent destination, or a value row in the account or character bank. */
[[nodiscard]] bool saved_pursuit_destination(const pursuits::ValueReference& reference) noexcept {
    if (reference.index == pursuits::kAbsent) {
        return !reference.bound;
    }
    return reference.bound && reference.source.opcode == unlocks::Opcode::loadValue
           && unlocks::valid(reference.source)
           && (reference.source.bank == unlocks::Bank::account
               || reference.source.bank == unlocks::Bank::character);
}

/** Names the most specific cause among the objectives that could not be read. */
[[nodiscard]] PursuitCompletionReason
objective_source_reason(const PursuitCompletionStatus& status) noexcept {
    bool external = false;
    for (std::size_t i = 0; i < status.objectiveCount; ++i) {
        const PursuitObjectiveUnknown unknown = status.objectives[i].unknown;
        if (unknown == PursuitObjectiveUnknown::itemProgressNotSaved) {
            return PursuitCompletionReason::itemProgressNotSaved;
        }
        external = external || unknown == PursuitObjectiveUnknown::externalValue;
    }
    return external ? PursuitCompletionReason::externalValue
                    : PursuitCompletionReason::objectiveSourceUnavailable;
}

/** Effect problems come first, so they stay visible even when every objective is complete. */
[[nodiscard]] PursuitCompletionReason
pursuit_completion_reason(const PursuitCompletionStatus& status,
                          const pursuits::Definition& definition,
                          bool operationsAbsent,
                          const CompletionValue& completion) noexcept {
    if (!operationsAbsent) {
        return PursuitCompletionReason::operationBodyUnavailable;
    }
    if (!status.effectsKnown) {
        return PursuitCompletionReason::valueDestinationUnavailable;
    }
    if (definition.suppressCompletion) {
        return PursuitCompletionReason::sweepSuppressed;
    }
    if (status.objectivesKnown && !status.objectivesComplete) {
        return PursuitCompletionReason::objectivesIncomplete;
    }
    if (completion.known && completion.value != 0) {
        return PursuitCompletionReason::valueAlreadyNonzero;
    }
    if (!status.objectivesKnown) {
        return objective_source_reason(status);
    }
    if (!completion.known) {
        return completion.external ? PursuitCompletionReason::externalValue
                                   : PursuitCompletionReason::valueSourceUnavailable;
    }
    return definition.automaticCompletion ? PursuitCompletionReason::automaticConditionsMet
                                          : PursuitCompletionReason::manualActionNotEstablished;
}

} // namespace

/** Evaluates all declared objectives before reporting aggregate eligibility or missing effects. */
PursuitCompletionStatus
inspect_pursuit_completion(const pursuits::Definition& definition,
                           const reward_resolution::Context& context) noexcept {
    PursuitCompletionStatus status{};
    if (!definition.readable || definition.objectiveCount > status.objectives.size()) {
        return status;
    }
    inspect_pursuit_objectives(definition, context, status);
    const CompletionValue completion = read_completion_value(definition.completionValue, context);
    // Either condition known to fail decides the result, even when the other cannot be read.
    status.conditionsKnown = (status.objectivesKnown && completion.known)
                             || (status.objectivesKnown && !status.objectivesComplete)
                             || (completion.known && completion.value != 0);
    status.eligible = status.objectivesComplete && completion.known
                      && !definition.suppressCompletion && completion.value == 0;
    const bool operationsAbsent = absent_pursuit_operation(definition.secondaryOperation)
                                  && absent_pursuit_operation(definition.primaryOperation);
    status.effectsKnown =
        operationsAbsent
        && (definition.suppressCompletion || saved_pursuit_destination(definition.completionValue))
        && saved_pursuit_destination(definition.completionContextValue);
    status.reason = pursuit_completion_reason(status, definition, operationsAbsent, completion);
    return status;
}

} // namespace sunrise::state
