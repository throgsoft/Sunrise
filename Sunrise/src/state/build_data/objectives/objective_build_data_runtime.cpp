#include <algorithm>
#include <array>
#include <mutex>

#include "../runtime.h"

namespace sunrise::state::build_data {
namespace {
std::mutex g_objectiveMutex;
std::array<objectives::Definition, objectives::kDefinitionCapacity> g_objectives{};
std::size_t g_objectiveCount{};
} // namespace

bool objective_definitions_ready() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    return g_objectiveCount != 0;
}
std::size_t objective_definition_count() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    return g_objectiveCount;
}
bool publish_objective_definitions(std::span<const objectives::Definition> rows) noexcept {
    if (rows.empty() || rows.size() > g_objectives.size()) return false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].definitionIndex != i) return false;
    }
    const std::lock_guard lock(g_objectiveMutex);
    std::copy(rows.begin(), rows.end(), g_objectives.begin());
    g_objectiveCount = rows.size();
    return true;
}
bool find_objective_definition(std::uint16_t index, objectives::Definition& output) noexcept {
    output = {};
    const std::lock_guard lock(g_objectiveMutex);
    if (index >= g_objectiveCount) return false;
    output = g_objectives[index];
    return true;
}
void clear_objective_definitions() noexcept {
    const std::lock_guard lock(g_objectiveMutex);
    g_objectives = {};
    g_objectiveCount = 0;
}
} // namespace sunrise::state::build_data
