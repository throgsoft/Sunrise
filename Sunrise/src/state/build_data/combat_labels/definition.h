#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::combat_labels {
inline constexpr std::uint32_t kRootClass = 0x808094B4U;
inline constexpr std::uint32_t kGroupClass = 0x808094BEU;
struct Group {
    std::uint32_t nameHash{};
    std::array<std::byte, 40> mask{};
};
struct Range {
    std::uint16_t start{};
    std::uint8_t count{};
};
struct Catalog {
    std::array<std::uint32_t, 320> labels{};
    std::size_t count{};
    std::array<Group, 32> groups{};
    std::size_t groupCount{};
    std::array<Range, 32> ranges{};
    std::size_t rangeCount{};
};
struct Source {
    bool attributed{};
    std::uint32_t weaponClass{};
    bool weapon{}, melee{}, grenade{}, superAbility{}, precision{};
    /** Unique authored grenade/Super label; zero when absent or ambiguous. */
    std::uint32_t abilityLabelHash{};
    /** The identical unique player label in both source and actor masks. */
    std::uint32_t playerClassHash{};
    /** Authored ability-group membership with no weapon label; ordinary melee is not inferred. */
    bool ability{};
};
/** Only the reviewed Scorn victim modifier is classified; unknown preserves caller fallback. */
enum class VictimRace : std::uint8_t { unknown, scorn, invalid };
/** Caller verifies the package's kRootClass; payload contains no package class identity. */
bool parse(std::span<const std::byte> blob, Catalog& output) noexcept;
} // namespace sunrise::state::build_data::combat_labels
