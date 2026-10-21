#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "../../state/account/account_state.h"
#include "../../state/account/inventory/inventory_edit.h"

namespace sunrise::server::bap {

/** The account, selected character and package content an operation was submitted against. */
struct InvestmentTarget {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t contentHash{};

    bool operator==(const InvestmentTarget&) const = default;
};

/** Grants one installed item through the lane that grants world rewards. */
struct GrantItemOperation {
    std::uint32_t definitionHash{};
    std::uint32_t quantity{};

    bool operator==(const GrantItemOperation&) const = default;
};

/** Replaces the seasonal XP total the editor read. */
struct SeasonalExperienceOperation {
    std::int32_t expected{};
    std::int32_t replacement{};

    bool operator==(const SeasonalExperienceOperation&) const = default;
};

/** Claims one pass row through the native opcode-2400 handler. */
struct SeasonPassClaimOperation {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};

    bool operator==(const SeasonPassClaimOperation&) const = default;
};

/** Clears one claimed pass row's claim flag. */
struct SeasonPassResetOperation {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};

    bool operator==(const SeasonPassResetOperation&) const = default;
};

/** Replaces the saved row one objective of one held pursuit reads. */
struct ObjectiveProgressOperation {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};
    std::uint32_t objectiveHash{};
    std::uint8_t ordinal{};
    std::int32_t expected{};
    std::int32_t value{};

    bool operator==(const ObjectiveProgressOperation&) const = default;
};

/** Sets every incomplete objective of one held pursuit to its target. */
struct ObjectiveCompletionOperation {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};

    bool operator==(const ObjectiveCompletionOperation&) const = default;
};

/** What one operation changes, in the order of InvestmentRequest's alternatives. */
enum class InvestmentOperationKind : std::uint8_t {
    grantItem,
    setQuantity,
    setSeasonalExperience,
    claimSeasonPassReward,
    resetSeasonPassClaim,
    setPursuitObjective,
    completePursuitObjectives,
};

/** One operation's request; each alternative's position is its InvestmentOperationKind. */
using InvestmentRequest = std::variant<GrantItemOperation,
                                       state::account::inventory::QuantityEdit,
                                       SeasonalExperienceOperation,
                                       SeasonPassClaimOperation,
                                       SeasonPassResetOperation,
                                       ObjectiveProgressOperation,
                                       ObjectiveCompletionOperation>;

/** One editor operation, copied out of the editor mapping before it is submitted. */
struct InvestmentOperation {
    /** Editor-chosen and nonzero; resubmitting the same identity never runs it twice. */
    std::uint64_t id{};
    InvestmentTarget target{};
    InvestmentRequest request{};

    bool operator==(const InvestmentOperation&) const = default;
};

/** Where one operation stands. Refused and both committed states are final. */
enum class InvestmentReceiptState : std::uint8_t {
    queued,
    deferred,
    refused,
    published,
    /** Committed, but its frame failed, so every Family-4 peer resyncs the account instead. */
    committedUnpublished,
};

/** What a queued or deferred operation is waiting for. */
enum class InvestmentWait : std::uint8_t {
    /** Nothing named, including an in-game presentation the bound peer is still playing. */
    none,
    publicationPeer,
    accountResync,
    catalog,
};

/** One operation's retained outcome. */
struct InvestmentReceipt {
    std::uint64_t operationId{};
    InvestmentTarget target{};
    InvestmentOperationKind kind{};
    InvestmentReceiptState state{};
    InvestmentWait wait{};
    /** Static refusal label, or the frame failure a committed operation was left with. */
    const char* reason{""};
    /** Family-4 version of the frame that published the operation; zero until published. */
    std::int32_t family4Version{};
    /** Inventory or unlock rows granted or edited; one for an XP total or a pass flag. */
    std::uint32_t changedRows{};
    std::uint64_t submittedTick{};
    /** Zero while the operation is pending. */
    std::uint64_t finishedTick{};
};

/** How a submission was taken. */
enum class InvestmentSubmission : std::uint8_t {
    /** The receipt is retained; its state says whether the operation was queued or refused. */
    accepted,
    /** The identity was already submitted with the same request; nothing ran again. */
    existing,
    /** The identity was already submitted with another request; nothing changed. */
    reused,
    /** No storage was left, so nothing was retained. */
    refused,
};

/** One character the account holds. */
struct InvestmentCharacter {
    std::uint64_t soid{};
    state::CharacterClass characterClass{};
    bool selected{};
};

/** The live account an editor binds to, and whether a peer can publish to it. */
struct InvestmentContext {
    InvestmentTarget live{};
    std::array<InvestmentCharacter, state::kCharacterCapacity> characters{};
    std::size_t characterCount{};
    /** Saved-account revision read with the account. */
    std::uint64_t revision{};
    bool peerPresent{};
    bool peerOwnsAccount{};
    bool peerResyncArmed{};
};

/** @return The kind the operation's request names. */
[[nodiscard]] InvestmentOperationKind
investment_operation_kind(const InvestmentOperation& operation) noexcept;

/**
 * Reads the live account under the session lock.
 * @return False when no valid account is loaded.
 */
[[nodiscard]] bool investment_context(InvestmentContext& output) noexcept;

/**
 * Validates an operation without drawing or writing, and retains its receipt until acknowledged.
 * An operation refused here is retained too, unless no storage was left for it.
 * @param operation Operation with a nonzero identity.
 * @param editorPid Process id of the editor that submitted it.
 * @param receipt Receives the new receipt, or the one already retained under that identity.
 */
[[nodiscard]] InvestmentSubmission submit_investment_operation(const InvestmentOperation& operation,
                                                               std::uint32_t editorPid,
                                                               InvestmentReceipt& receipt) noexcept;

/** @return False with an empty receipt when no receipt is retained under that identity. */
[[nodiscard]] bool investment_receipt(std::uint64_t operationId,
                                      InvestmentReceipt& receipt) noexcept;

/**
 * Forgets a finished receipt.
 * @param receipt Receives the receipt; empty when none is retained under that identity.
 * @return False while the operation is still pending, or when no receipt is retained.
 */
[[nodiscard]] bool acknowledge_investment_receipt(std::uint64_t operationId,
                                                  InvestmentReceipt& receipt) noexcept;

/**
 * Refuses a queued or deferred operation as withdrawn; a finished one is left as it is.
 * @param receipt Receives the resulting receipt; empty when none is retained.
 * @return False when no receipt is retained under that identity.
 */
[[nodiscard]] bool withdraw_investment_operation(std::uint64_t operationId,
                                                 InvestmentReceipt& receipt) noexcept;

/**
 * Refuses every queued or deferred operation, because the editor that would read how it ended
 * is gone. Finished receipts stay for that editor to read when it returns.
 * @param reason Static label each refused receipt carries.
 */
void refuse_pending_investment_operations(const char* reason) noexcept;

/**
 * Forgets the receipts of every editor that has exited; only that editor knew their ids.
 * @param editorExited True once no process runs under the editor's process id.
 */
void forget_exited_editor_receipts(bool (*editorExited)(std::uint32_t editorPid) noexcept) noexcept;

} // namespace sunrise::server::bap
