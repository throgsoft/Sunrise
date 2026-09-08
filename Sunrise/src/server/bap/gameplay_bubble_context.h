#pragma once
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/activity_sdk/generated_world/runtime.h"
#include "../../state/bounties/named_area_rules.h"
#include "internal.h"

namespace sunrise::server::bap {
/** Read at authenticated ingress under the connection lock. Pending regions never supply credit. */
inline std::optional<state::bounties::areas::BubbleFact>
gameplay_bubble_locked(const Session& owner) noexcept {
    namespace sdk = state::activity_sdk;
    namespace world = sdk::generated_world;
    namespace membership = state::activity::membership;
    const auto region = membership::instantiated_region(
        membership::reported_placement(owner.activity.session.sessionId));
    if (region < 0) return {};
    sdk::BoundView bound{};
    if (sdk::resolve(
            sdk::snapshot(), {owner.activity.session, 1, owner.activity.bindingGeneration}, bound)
        != sdk::Status::ready)
        return {};
    world::GeneratedWorldView view{};
    if (world::resolve(bound, view) != world::BindStatus::ready || !view.snapshot()) return {};
    const auto& snapshot = *view.snapshot();
    if (snapshot.scenarioTag != view.scenario_tag()) return {};
    const state::build_data::scriptables::State* selected{};
    for (const auto& state : snapshot.states) {
        if (std::uint64_t{state.sliceSetIndex} + state.index != static_cast<std::uint32_t>(region))
            continue;
        if (selected) return {};
        selected = &state;
    }
    if (!selected || !selected->resolved || !selected->enabled
        || selected->bubbleRow >= snapshot.bubbles.size())
        return {};
    const auto& bubble = snapshot.bubbles[selected->bubbleRow];
    if (!bubble.nameHash || bubble.index != selected->bubbleRow
        || selected->index >= bubble.stateCount || bubble.firstState > snapshot.states.size()
        || bubble.stateCount > snapshot.states.size() - bubble.firstState
        || &snapshot.states[bubble.firstState + selected->index] != selected)
        return {};
    return state::bounties::areas::BubbleFact{view.scenario_tag(), bubble.nameHash};
}
} // namespace sunrise::server::bap
