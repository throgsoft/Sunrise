#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state {

/** The held pursuit an editor read; another character, instance, hash or serial refuses it. */
struct PursuitIdentity {
    std::uint64_t characterSoid{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
};

/** The saved objects one pursuit edit changed, for the caller's refresh. */
struct PursuitEditChange {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    /** The selected character's slot. */
    std::size_t characterIndex{};
    /** A bank the account object publishes changed, so that object is refreshed too. */
    bool accountRows{};
    /** Saved unlock rows written. */
    std::uint32_t changedRows{};
};

/**
 * Replaces the one saved row an objective's progress reads, compared with the value the editor
 * read. Item-lane objectives and bounties are refused: this server saves neither per-instance
 * progress nor expiry.
 * @param identity The held pursuit, on the selected character.
 * @param ordinal Objective position in the held pursuit's installed contract.
 * @param objectiveHash Definition hash the editor read at that position.
 * @param expected The row as the evaluator reads it: 0 or 1 for a flag.
 * @param value Replacement; 0 or 1 for a flag.
 * @param change Receives the changed identities on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool set_pursuit_objective(const PursuitIdentity& identity,
                                         std::uint8_t ordinal,
                                         std::uint32_t objectiveHash,
                                         std::int32_t expected,
                                         std::int32_t value,
                                         PursuitEditChange& change,
                                         const char** refusal = nullptr) noexcept;

/**
 * Writes every required, incomplete objective's saved row to its target in one savepoint.
 * Never redeems the pursuit; its completion value and operations are untouched.
 * @param identity The held pursuit, on the selected character.
 * @param change Receives the changed identities on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool complete_pursuit_objectives(const PursuitIdentity& identity,
                                               PursuitEditChange& change,
                                               const char** refusal = nullptr) noexcept;

} // namespace sunrise::state
