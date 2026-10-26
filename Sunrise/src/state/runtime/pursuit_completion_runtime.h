#pragma once

#include <array>
#include <cstdint>

#include "../build_data/pursuits/definition.h"

namespace sunrise::state::reward_resolution {
struct Context;
}

namespace sunrise::state {

/** Why an objective's value or target could not be read from the saved banks. */
enum class PursuitObjectiveUnknown : std::uint8_t {
    none,
    definitionUnavailable,
    expressionUnreadable,
    adjustmentUnreadable,
    /** Per-item progress lives in the instance, which this server does not save. */
    itemProgressNotSaved,
    /** The objective reads an external value, which the saved banks do not hold. */
    externalValue,
    sourceUnavailable,
};

/** Why a pursuit's completion stands where it does, in the game's priority order. */
enum class PursuitCompletionReason : std::uint8_t {
    contractUnavailable,
    operationBodyUnavailable,
    valueDestinationUnavailable,
    sweepSuppressed,
    objectivesIncomplete,
    valueAlreadyNonzero,
    objectiveSourceUnavailable,
    valueSourceUnavailable,
    automaticConditionsMet,
    manualActionNotEstablished,
    itemProgressNotSaved,
    externalValue,
};

/** One declared objective, evaluated with its installed comparison. */
struct PursuitObjectiveStatus {
    std::uint16_t index{build_data::pursuits::kAbsent};
    std::uint32_t hash{};
    bool known{};
    bool complete{};
    bool skipped{};
    std::int32_t value{};
    std::int32_t target{};
    PursuitObjectiveUnknown unknown{PursuitObjectiveUnknown::none};
};

/** Eligibility describes conditions only. It does not permit a claim or promise a grant. */
struct PursuitCompletionStatus {
    /** The objectives and the completion gate together give a determinate result. */
    bool conditionsKnown{};
    /** The objective aggregate alone is determinate, ignoring the completion gate. */
    bool objectivesKnown{};
    /** In any-objective mode, one complete objective proves this even when others are unknown. */
    bool objectivesComplete{};
    bool eligible{};
    /** No operation body is needed, and the server can save every value completion writes. */
    bool effectsKnown{};
    std::uint8_t objectiveCount{};
    std::array<PursuitObjectiveStatus, build_data::pursuits::kObjectiveCapacity> objectives{};
    /**
     * The reason that decides the result. When an effect problem decides it, the condition fields
     * still hold their evaluated values.
     */
    PursuitCompletionReason reason{PursuitCompletionReason::contractUnavailable};
};

/**
 * Inspects completion against the saved banks without writing or drawing.
 * The caller matches the contract's item index and hash to the held instance.
 * @param definition Installed completion contract.
 * @param context Saved banks and class of the character holding the pursuit.
 * @return The per-objective and aggregate status, with the reason that decides it.
 */
[[nodiscard]] PursuitCompletionStatus
inspect_pursuit_completion(const build_data::pursuits::Definition& definition,
                           const reward_resolution::Context& context) noexcept;

} // namespace sunrise::state
