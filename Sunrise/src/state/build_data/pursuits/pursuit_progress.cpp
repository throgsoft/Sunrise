#include "pursuit_progress.h"

#include "../../account/inventory/inventory_state.h"
#include "../items/details/definition.h"
#include "../objectives/definition.h"
#include "../runtime.h"

namespace sunrise::state::build_data::pursuits {

/** Measures one held pursuit against the objectives its definition declares. */
Progress measure(std::uint16_t itemDefinitionIndex, std::span<const std::int32_t> lanes) noexcept {
    Progress progress{};
    items::details::Definition detail{};
    if (!find_configured_item_detail(itemDefinitionIndex, detail) || detail.objectiveCount == 0
        || detail.objectiveCount > detail.objectiveIndices.size()) {
        return progress;
    }
    progress.objectiveCount = detail.objectiveCount;
    progress.resolved = true;
    for (std::size_t entry = 0; entry < detail.objectiveCount; ++entry) {
        objectives::Definition objective{};
        // Lane zero is the Unix expiry; objective ordinals start in lane one.
        const std::size_t lane = entry + account::inventory::kItemObjectiveLaneBase;
        if (lane >= lanes.size()
            || !find_objective_definition(detail.objectiveIndices[entry], objective)
            || objective.completionValue <= 0) {
            progress.resolved = false;
            continue;
        }
        if (lanes[lane] >= objective.completionValue) {
            ++progress.completeCount;
        }
    }
    return progress;
}

/** Reports whether a held pursuit has finished every objective it declares. */
bool complete(std::uint16_t itemDefinitionIndex, std::span<const std::int32_t> lanes) noexcept {
    const Progress progress = measure(itemDefinitionIndex, lanes);
    return progress.objectiveCount != 0 && progress.resolved
           && progress.completeCount == progress.objectiveCount;
}

} // namespace sunrise::state::build_data::pursuits
