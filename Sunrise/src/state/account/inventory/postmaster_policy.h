#pragma once

#include "inventory_state.h"
#include "../../build_data/items/details/definition.h"

namespace sunrise::state::account::inventory {

/** Missing extraction is unknown, not proof that an acquire effect is absent. */
[[nodiscard]] inline bool postmaster_acquire_effect_absent(
    const build_data::items::details::Definition& detail) noexcept {
    return detail.acquireEffectIndex.has_value() && *detail.acquireEffectIndex == 0xFFFFU;
}

/** Restricted whole-instance subset; timed pursuits, rewards and stacks stay unsupported. */
[[nodiscard]] inline bool postmaster_supported(
    const Item& item, const build_data::items::details::Definition& detail) noexcept {
    return valid(item) && item.quantity == 1
           && detail.instancedDefinitionState
                  == build_data::items::details::InstancedDefinitionState::instanced
           && detail.equipmentSlot.has_value() && *detail.equipmentSlot > 0
           && static_cast<std::size_t>(*detail.equipmentSlot)
                  < build_data::items::details::kEquipmentSlotCount
           && detail.objectiveCount == 0 && detail.lifetimeSeconds == 0 && detail.rewardCount == 0
           && postmaster_acquire_effect_absent(detail);
}

} // namespace sunrise::state::account::inventory
