#include <algorithm>
#include <mutex>
#include <vector>

#include "../runtime.h"

namespace sunrise::state::build_data {
namespace {

/** Process-local: the table is read from the installed packages on every startup. */
std::mutex g_objectiveMutex;
std::vector<objectives::Definition> g_objectives;

/** A per-item source is one external value read; no other row names an item value. */
[[nodiscard]] bool valid_objective(const objectives::Definition& row, std::size_t index) noexcept {
    const bool itemSource = !row.itemProgress
                            || (row.readable && row.expression.size() == 1
                                && row.expression.front().opcode == unlocks::Opcode::loadValue
                                && row.expression.front().bank == unlocks::Bank::external);
    return row.definitionIndex == index && row.readable != row.expression.empty() && itemSource
           && row.itemProgress == (row.itemValueIndex != objectives::kNoItemValue)
           && (row.itemProgress || row.itemValueMode == objectives::ItemValueMode::context)
           && (row.adjustmentReadable || row.targetAdjustment.empty())
           && std::all_of(row.expression.begin(), row.expression.end(), unlocks::valid)
           && std::all_of(row.targetAdjustment.begin(), row.targetAdjustment.end(), unlocks::valid);
}

} // namespace

bool objective_definitions_ready() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    return !g_objectives.empty();
}

std::size_t objective_definition_count() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    return g_objectives.size();
}

bool publish_objective_definitions(std::span<const objectives::Definition> rows) noexcept {
    if (rows.empty() || rows.size() > objectives::kDefinitionCapacity) {
        return false;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!valid_objective(rows[i], i)) {
            return false;
        }
    }
    try {
        std::vector<objectives::Definition> prepared(rows.begin(), rows.end());
        const std::lock_guard lock(g_objectiveMutex);
        g_objectives.swap(prepared);
        return true;
    } catch (...) {
        return false;
    }
}

bool find_objective_definition(std::uint16_t index, objectives::Definition& output) noexcept {
    output = {};
    const std::lock_guard lock(g_objectiveMutex);
    if (index >= g_objectives.size()) {
        return false;
    }
    try {
        output = g_objectives[index];
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

void clear_objective_definitions() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    g_objectives = {};
}

} // namespace sunrise::state::build_data
