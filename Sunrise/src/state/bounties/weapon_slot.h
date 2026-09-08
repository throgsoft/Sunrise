#pragma once

#include <cstdint>

namespace sunrise::state::bounties::gameplay {

/** Native equipment-slot identity; independent of ammo type, damage and item hash. */
enum class WeaponSlot : std::uint8_t { kinetic = 7, energy = 8, heavy = 9 };

[[nodiscard]] constexpr bool valid_weapon_slot(WeaponSlot slot) noexcept {
    return slot == WeaponSlot::kinetic || slot == WeaponSlot::energy || slot == WeaponSlot::heavy;
}

} // namespace sunrise::state::bounties::gameplay
