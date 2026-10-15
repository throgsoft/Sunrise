#include <algorithm>
#include <mutex>
#include <vector>

#include "../../../core/logging/log.h"
#include "../runtime.h"

namespace sunrise::state::build_data {
namespace {

/** Process-local: contracts are rebuilt from the installed packages on every startup. */
std::mutex g_pursuitMutex;
std::vector<pursuits::Definition> g_pursuitDefinitions;
bool g_pursuitsReady{};
std::uint64_t g_pursuitsRevision{};

/** An unbound native reference remains inspectable without pretending it is a saved value. */
[[nodiscard]] bool valid_pursuit_value(const pursuits::ValueReference& value) noexcept {
    if (value.index == pursuits::kAbsent) {
        return !value.bound;
    }
    return !value.bound
           || (value.source.opcode == unlocks::Opcode::loadValue && unlocks::valid(value.source));
}

/** Only an authored absent index can carry the absent status. */
[[nodiscard]] bool valid_pursuit_operation(const pursuits::OperationReference& operation) noexcept {
    switch (operation.availability) {
    case pursuits::OperationAvailability::absent:
        return operation.index == pursuits::kAbsent;
    case pursuits::OperationAvailability::missingTable:
    case pursuits::OperationAvailability::unresolvedBody:
    case pursuits::OperationAvailability::invalidReference:
        return operation.index != pursuits::kAbsent;
    }
    return false;
}

/** A shared condition index is readable either as a nonempty program or not at all. */
[[nodiscard]] bool valid_pursuit_reward(const pursuits::RewardPreview& reward) noexcept {
    items::Definition item{};
    if (!find_item_definition_index(reward.itemIndex, item) || reward.quantity < 0
        || !std::all_of(reward.condition.begin(), reward.condition.end(), unlocks::valid)) {
        return false;
    }
    if (reward.conditionIndex == pursuits::kAbsent) {
        return reward.conditionReadable && reward.condition.empty();
    }
    return reward.conditionReadable != reward.condition.empty();
}

/** Validates every declared edge before publishing the complete immutable mapping. */
[[nodiscard]] bool valid_pursuit(const pursuits::Definition& row) noexcept {
    items::Definition item{};
    if (!find_item_definition_index(row.definitionIndex, item)
        || item.definitionHash != row.definitionHash || row.lifetimeSeconds < 0
        || row.objectives.size() > pursuits::kObjectiveCapacity
        || row.rewards.size() > pursuits::kRewardCapacity
        || !valid_pursuit_value(row.completionValue)
        || !valid_pursuit_value(row.completionContextValue)
        || !valid_pursuit_operation(row.secondaryOperation)
        || !valid_pursuit_operation(row.primaryOperation)
        || (!row.rewardsReadable && !row.rewards.empty())) {
        return false;
    }
    for (const std::uint16_t index : row.objectives) {
        objectives::Definition objective{};
        if (!find_objective_definition(index, objective)) {
            return false;
        }
    }
    return std::all_of(row.rewards.begin(), row.rewards.end(), valid_pursuit_reward);
}

} // namespace

bool pursuit_definitions_ready() noexcept {
    const std::lock_guard lock(g_pursuitMutex);
    return g_pursuitsReady;
}

std::size_t pursuit_definition_count() noexcept {
    const std::lock_guard lock(g_pursuitMutex);
    return g_pursuitDefinitions.size();
}

std::uint64_t pursuit_definitions_revision() noexcept {
    const std::lock_guard lock(g_pursuitMutex);
    return g_pursuitsRevision;
}

bool publish_pursuit_definitions(std::span<const pursuits::Definition> rows) noexcept {
    if (!item_definitions_ready() || !objective_definitions_ready()
        || rows.size() > items::kDefinitionCapacity) {
        return false;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!valid_pursuit(rows[i])
            || (i != 0 && rows[i - 1].definitionIndex >= rows[i].definitionIndex)) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=pursuit_definitions result=rejected item=%u row=%zu total=%zu",
                              static_cast<unsigned>(rows[i].definitionIndex),
                              i,
                              rows.size());
            return false;
        }
    }
    try {
        std::vector<pursuits::Definition> prepared(rows.begin(), rows.end());
        const std::lock_guard lock(g_pursuitMutex);
        g_pursuitDefinitions.swap(prepared);
        g_pursuitsReady = true;
        ++g_pursuitsRevision;
        return true;
    } catch (...) {
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=pursuit_definitions result=fail reason=storage rows=%zu",
                          rows.size());
        return false;
    }
}

bool find_pursuit_definition(std::uint16_t index, pursuits::Definition& output) noexcept {
    output = {};
    const std::lock_guard lock(g_pursuitMutex);
    const auto found = std::lower_bound(g_pursuitDefinitions.begin(),
                                        g_pursuitDefinitions.end(),
                                        index,
                                        [](const auto& row, std::uint16_t wanted) noexcept {
                                            return row.definitionIndex < wanted;
                                        });
    if (!g_pursuitsReady || found == g_pursuitDefinitions.end()
        || found->definitionIndex != index) {
        return false;
    }
    try {
        output = *found;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

void clear_pursuit_definitions() noexcept {
    const std::lock_guard lock(g_pursuitMutex);
    g_pursuitDefinitions = {};
    g_pursuitsReady = false;
    ++g_pursuitsRevision;
}

} // namespace sunrise::state::build_data
