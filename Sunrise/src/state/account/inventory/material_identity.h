#pragma once

#include <cstdint>

namespace sunrise::state::account::inventory {

/** Installed item identities used by wallet, stack, and discard policies. */
inline constexpr std::uint32_t kGlimmerHash = 3159615086U;
inline constexpr std::uint32_t kEnhancementCoreHash = 3853748946U;
inline constexpr std::uint32_t kGunsmithMaterialsHash = 685157383U;

} // namespace sunrise::state::account::inventory
