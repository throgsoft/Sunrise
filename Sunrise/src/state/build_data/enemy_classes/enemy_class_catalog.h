#pragma once
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::enemy_classes {
struct Catalog;
enum class Lookup : std::uint8_t { unavailable, unknown, enemy, nonEnemy, invalid };
bool replace(const Catalog& catalog) noexcept;
std::size_t count() noexcept;
/** Resolve the native investment class index, with direct class metadata as a cross-check.
 * A missing direct class can be an authored alias (e.g. Heavy Shank -> Shank).
 * A contradictory race, non-enemy class or invalid index must never use a fallback.
 */
Lookup classify(std::int16_t index, std::uint32_t classHash, std::uint32_t& race) noexcept;
} // namespace sunrise::state::build_data::enemy_classes
