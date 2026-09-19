#pragma once

#include "../../build_data/inventory/buckets/definition.h"
#include "../../build_data/items/details/definition.h"
#include "inventory_state.h"

namespace sunrise::state::account::inventory {

/** Missing extraction is unknown, not proof that an acquire effect is absent. */
[[nodiscard]] inline bool
postmaster_acquire_effect_absent(const build_data::items::details::Definition& detail) noexcept {
    return detail.acquireEffectIndex.has_value() && *detail.acquireEffectIndex == 0xFFFFU;
}

/**
 * An engram carries no equipment slot and declares the effect it plays when decrypted, so
 * neither gear test fits one. Lost Items still holds engrams exactly as it holds gear.
 */
[[nodiscard]] inline bool
postmaster_engram(const build_data::items::details::Definition& detail) noexcept {
    return detail.bucketId == build_data::inventory::buckets::kEngramBucketId
           && !detail.equipmentSlot.has_value();
}

/** Restricted whole-instance subset; timed pursuits, rewards and stacks stay unsupported. */
[[nodiscard]] inline bool
postmaster_supported(const Item& item,
                     const build_data::items::details::Definition& detail) noexcept {
    const bool engram = postmaster_engram(detail);
    const bool equippable = detail.equipmentSlot.has_value() && *detail.equipmentSlot > 0
                            && static_cast<std::size_t>(*detail.equipmentSlot)
                                   < build_data::items::details::kEquipmentSlotCount;
    return valid(item) && item.quantity == 1
           && detail.instancedDefinitionState
                  == build_data::items::details::InstancedDefinitionState::instanced
           && (engram || equippable) && detail.objectiveCount == 0 && detail.lifetimeSeconds == 0
           && detail.rewardCount == 0 && (engram || postmaster_acquire_effect_absent(detail));
}

} // namespace sunrise::state::account::inventory
