#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "../../../../../state/build_data/sobjects/sobject_catalog.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts::world_reward {

enum class GenericReward { none, moonRabbit };

/** A shared interaction definition is not a collectible identity. In particular, 3539 also
 * reports orb pickups in the Dreaming City. Cat rewards require a separately proven statue/gift
 * transaction and cannot be inferred from the destination, primary definition, or player position.
 */
template <class Find>
[[nodiscard]] GenericReward generic_interaction(
    std::uint32_t target, const state::build_data::sobjects::Definition* primary,
    std::span<const std::uint32_t> extras, std::string_view package, Find&& find) noexcept {
    if (target != 3539U || !primary || primary->typeCode != 2
        || primary->nameHash != 0x7A0FD954U || primary->lane4 != 0x0011FFFFU
        || package != "luna_freeroam") {
        return GenericReward::none;
    }
    // Preserve the existing exact Moon statue ordinal identification.
    for (const auto extra : extras) {
        state::build_data::sobjects::Definition statue{};
        if (extra >= state::build_data::sobjects::kDefinitionCapacity
            || !find(static_cast<std::uint16_t>(extra), statue)
            || statue.typeCode != 2 || statue.recordRow() != 0xFFFFU) {
            continue;
        }
        const auto ordinal = statue.loreObjectOrdinal();
        if (ordinal >= 3297U && ordinal <= 3305U) {
            return GenericReward::moonRabbit;
        }
    }
    return GenericReward::none;
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts::world_reward
