#include "package_pursuit_build.h"

#include <utility>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace pursuits = state::build_data::pursuits;

/**
 * Root slot 84 holds the operation table that completion references name, the same table reward
 * entries name as supplemental rows.
 */
constexpr std::size_t kCompletionOperationTableSlot = 84;
constexpr std::uint32_t kCompletionOperationTableClass = 0x808078BBU;
constexpr std::size_t kCompletionOperationRowStride = 0x120;

static_assert(pursuits::kObjectiveCapacity == tables::items::kObjectiveCapacity);
static_assert(pursuits::kRewardCapacity == tables::items::kRewardCapacity);
static_assert(pursuits::kAbsent == tables::items::kAbsentReference);

/** Set once the installed data refuses an objective or pursuit table; no retry changes that. */
bool g_pursuitsUnsupported = false;

} // namespace

void settle_pursuits_unsupported(const char* reason) noexcept {
    if (g_pursuitsUnsupported) {
        return;
    }
    g_pursuitsUnsupported = true;
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=pursuits result=unsupported reason=%s",
                      reason);
}

/** @return True when the pursuit contracts are published or can never be. */
bool pursuit_catalog_settled() noexcept {
    return state::build_data::pursuit_definitions_ready() || g_pursuitsUnsupported;
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
    operationCount_ = 0;
    operationTablePresent_ = false;
    std::uint32_t tag = 0;
    if (!load_pursuit_conditions(conditions_, source, scratch, root, maps)) {
        return false;
    }
    if (!tables::slot_tag(root, kCompletionOperationTableSlot, tag)) {
        settle_pursuits_unsupported("operation_slot");
        return false;
    }
    if (tables::package_of(tag) != tables::kAbsentPackageId) {
        std::vector<std::byte> blob;
        std::uint32_t tableClass = 0;
        tables::Array operations{};
        if (!reader::read_tag(source, scratch, tag, blob, tableClass)) {
            return false;
        }
        if (tableClass != kCompletionOperationTableClass
            || !tables::find_array_at(blob, tables::kTableArrayDescriptor, operations)
            || operations.dataOffset > blob.size()
            || operations.count
                   > (blob.size() - operations.dataOffset) / kCompletionOperationRowStride) {
            settle_pursuits_unsupported("operation_schema");
            return false;
        }
        operationTablePresent_ = true;
        operationCount_ = operations.count;
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
    // Only a pursuit declares completion metadata, or fails to read it.
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
            output.objectives.assign(row.objectiveIndices,
                                     row.objectiveIndices + row.objectiveCount);
            output.completionValue = value(row.completionValueIndex);
            output.completionContextValue = value(row.completionContextValueIndex);
            output.secondaryOperation = operation(row.secondaryOperationIndex);
            output.primaryOperation = operation(row.primaryOperationIndex);
        }
        output.rewardsReadable = output.readable && row.rewardMetadataValid;
        if (output.rewardsReadable) {
            for (std::size_t i = 0; i < row.rewardCount; ++i) {
                const tables::items::Reward& native = row.rewards[i];
                pursuits::RewardPreview reward{};
                reward.itemIndex = native.itemIndex;
                reward.quantity = native.quantity;
                reward.conditionIndex = native.conditionIndex;
                BankFailure failure{};
                reward.conditionReadable =
                    native.conditionIndex == pursuits::kAbsent
                    || conditions_.read_index(native.conditionIndex, reward.condition, failure);
                if (failure != BankFailure::none) {
                    return false;
                }
                output.rewards.push_back(std::move(reward));
            }
        }
        rows_.push_back(std::move(output));
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

void PursuitBuild::publish() noexcept {
    // Contracts check every edge against both tables, so they wait for them on a later pass.
    if (!ready_ || !state::build_data::item_definitions_ready()
        || !state::build_data::objective_definitions_ready()) {
        return;
    }
    std::size_t unresolved = 0;
    for (const pursuits::Definition& row : rows_) {
        unresolved += row.primaryOperation.index != pursuits::kAbsent
                      || row.secondaryOperation.index != pursuits::kAbsent;
    }
    const bool published = state::build_data::publish_pursuit_definitions(rows_);
    const std::size_t rowCount = rows_.size();
    // State keeps its own copy; this pass never walks for pursuits again.
    rows_ = {};
    ready_ = false;
    if (!published) {
        settle_pursuits_unsupported("publish");
        return;
    }
    core::log::writef(core::log::Channel::client,
                      unreadCount_ == 0 ? core::log::Level::info : core::log::Level::warn,
                      "ev=pkg stage=pursuits result=ready rows=%zu unread=%zu first_unread=%u "
                      "unresolved_effects=%zu operation_table=%s",
                      rowCount,
                      unreadCount_,
                      static_cast<unsigned>(firstUnread_),
                      unresolved,
                      operationTablePresent_ ? "present" : "absent");
}

} // namespace sunrise::client::content::items::packages
