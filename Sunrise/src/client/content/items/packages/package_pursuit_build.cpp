#include "package_pursuit_build.h"

#include <algorithm>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace pursuits = state::build_data::pursuits;

static_assert(pursuits::kObjectiveCapacity == tables::items::kObjectiveCapacity);
static_assert(pursuits::kAbsent == tables::items::kAbsentReference);
static_assert(state::build_data::items::kPursuitBucketId == tables::items::kPursuitBucketId);

} // namespace

void settle_pursuits_unsupported(const char* reason) noexcept {
    if (state::build_data::pursuit_catalog_unsupported()) {
        return;
    }
    state::build_data::settle_pursuit_catalog_unsupported();
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=pursuits result=unsupported reason=%s",
                      reason);
}

/** @return True when the pursuit contracts are published or can never be. */
bool pursuit_catalog_settled() noexcept {
    return state::build_data::pursuit_definitions_ready()
           || state::build_data::pursuit_catalog_unsupported();
}

bool load_pursuit_conditions(RewardConditions& conditions,
                             const reader::Source& source,
                             reader::Scratch& scratch,
                             std::span<const std::byte> root,
                             const SlotMaps& maps) noexcept {
    if (conditions.load(source, scratch, root, maps)) {
        return true;
    }
    if (reward_definitions_settled() && !state::build_data::reward_definitions_ready()) {
        settle_pursuits_unsupported("condition_tables");
    }
    return false;
}

bool PursuitBuild::begin(const reader::Source& source,
                         reader::Scratch& scratch,
                         std::span<const std::byte> root,
                         const SlotMaps& maps) noexcept {
    ready_ = false;
    rows_.clear();
    unreadCount_ = 0;
    firstUnread_ = pursuits::kAbsent;
    readFailures_ = 0;
    firstReadFailure_ = pursuits::kAbsent;
    operationCount_ = 0;
    operationTablePresent_ = false;
    std::uint32_t tag = 0;
    if (!load_pursuit_conditions(conditions_, source, scratch, root, maps)) {
        return false;
    }
    if (!tables::slot_tag(root, tables::kOperationTableSlot, tag)) {
        settle_pursuits_unsupported("operation_slot");
        return false;
    }
    if (tables::package_of(tag) != tables::kAbsentPackageId) {
        std::vector<std::byte> blob;
        std::uint32_t tableClass = 0;
        tables::Array operations{};
        // The table only explains why an operation body is unresolved, so a failed read records
        // it missing. Retrying it would hold back every other investment-root domain.
        if (!reader::read_tag(source, scratch, tag, blob, tableClass)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=pkg stage=pursuits result=partial reason=operation_table_read");
        } else if (tableClass != tables::kOperationTableClass
                   || !tables::find_array_at(blob, tables::kTableArrayDescriptor, operations)
                   || operations.dataOffset > blob.size()
                   || operations.count
                          > (blob.size() - operations.dataOffset) / tables::kOperationRowStride) {
            settle_pursuits_unsupported("operation_schema");
            return false;
        } else {
            operationTablePresent_ = true;
            operationCount_ = operations.count;
        }
    }
    ready_ = true;
    return true;
}

pursuits::ValueReference PursuitBuild::value(std::uint16_t index) const noexcept {
    pursuits::ValueReference result{};
    result.index = index;
    result.bound = index != pursuits::kAbsent
                   && conditions_.bind(tables::kUnlockReadValueOpcode, index, result.source)
                   && state::unlocks::valid(result.source);
    return result;
}

pursuits::OperationReference PursuitBuild::operation(std::uint16_t index) const noexcept {
    using Status = pursuits::OperationAvailability;
    return {index,
            index == pursuits::kAbsent ? Status::absent
            : !operationTablePresent_  ? Status::missingTable
            : index >= operationCount_ ? Status::invalidReference
                                       : Status::unresolvedBody};
}

bool PursuitBuild::item(const tables::items::Row& row) noexcept {
    if (!ready_) {
        return false;
    }
    // Only pursuits declare completion metadata. An unreadable block gets an unreadable contract.
    if (!row.hasCompletionMetadata && row.objectiveMetadataValid) {
        return true;
    }
    try {
        pursuits::Definition output{};
        output.definitionIndex = row.definitionIndex;
        output.definitionHash = row.definitionHash;
        output.readable = row.objectiveMetadataValid && row.hasCompletionMetadata;
        output.lifetimeSeconds = row.lifetimeSeconds;
        if (output.readable) {
            output.suppressCompletion = row.suppressCompletion;
            output.requireAllObjectives = row.requireAllObjectives;
            output.requireNonOptionalCompletion = row.requireNonOptionalCompletion;
            output.automaticCompletion = row.automaticCompletion;
            std::copy_n(row.objectiveIndices, row.objectiveCount, output.objectives.begin());
            output.objectiveCount = row.objectiveCount;
            output.completionValue = value(row.completionValueIndex);
            output.completionContextValue = value(row.completionContextValueIndex);
            output.secondaryOperation = operation(row.secondaryOperationIndex);
            output.primaryOperation = operation(row.primaryOperationIndex);
        }
        rows_.push_back(output);
        return true;
    } catch (...) {
        return false;
    }
}

void PursuitBuild::unread(std::uint16_t index) noexcept {
    if (unreadCount_++ == 0) {
        firstUnread_ = index;
    }
}

void PursuitBuild::read_failed(std::uint16_t index) noexcept {
    if (readFailures_++ == 0) {
        firstReadFailure_ = index;
    }
}

void PursuitBuild::publish() noexcept {
    // Every contract edge is checked against the item and objective tables, so wait for both.
    if (!ready_ || !state::build_data::item_definitions_ready()
        || !state::build_data::objective_definitions_ready()) {
        return;
    }
    // A failed read or allocation left out contracts the data declares. Publishing now would cache
    // the catalog without them, and validating it could settle the domain unsupported over a
    // failure the data did not decide.
    if (readFailures_ != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=pursuits result=fail reason=item_read read_failures=%zu "
                          "first_index=%u",
                          readFailures_,
                          static_cast<unsigned>(firstReadFailure_));
        return;
    }
    // A row an earlier pass failed to read stays out of the published item catalog, so its
    // contract is pruned with it rather than settling the domain over an item State never holds.
    const std::size_t pruned = std::erase_if(rows_, [](const pursuits::Definition& row) noexcept {
        state::build_data::items::Definition item{};
        return !state::build_data::find_item_definition_index(row.definitionIndex, item);
    });
    std::size_t unresolved = 0;
    for (const pursuits::Definition& row : rows_) {
        unresolved += row.primaryOperation.index != pursuits::kAbsent
                      || row.secondaryOperation.index != pursuits::kAbsent;
    }
    const bool valid = state::build_data::valid_pursuit_definitions(rows_);
    // A cache write can still fail, so a refused publish walks again on a later pass.
    const bool published = valid && state::build_data::publish_pursuit_definitions(rows_);
    const std::size_t rowCount = rows_.size();
    // State keeps its own copy, so release the collected rows.
    rows_ = {};
    ready_ = false;
    if (!valid) {
        settle_pursuits_unsupported("contracts");
        return;
    }
    if (!published) {
        return;
    }
    core::log::writef(core::log::Channel::client,
                      unreadCount_ == 0 && pruned == 0 ? core::log::Level::info
                                                       : core::log::Level::warn,
                      "ev=pkg stage=pursuits result=ready rows=%zu unread=%zu first_unread=%u "
                      "pruned=%zu unresolved_effects=%zu operation_table=%s",
                      rowCount,
                      unreadCount_,
                      static_cast<unsigned>(firstUnread_),
                      pruned,
                      unresolved,
                      operationTablePresent_ ? "present" : "absent");
}

} // namespace sunrise::client::content::items::packages
