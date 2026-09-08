#include "catalog.h"

#include <mutex>
#include <string_view>

namespace sunrise::state::build_data::combat_labels {
namespace {
std::mutex g_mutex;
Catalog g_catalog{};
constexpr std::uint32_t hash(std::string_view name) noexcept {
    std::uint32_t result = 0x811C9DC5U;
    for (const unsigned char c : name)
        result = (result * 0x1000193U) ^ c;
    return result;
}
constexpr std::array required{hash("player"),
                              hash("combatant"),
                              hash("weapon"),
                              hash("melee"),
                              hash("grenade"),
                              hash("super"),
                              hash("ability")};
constexpr auto precisionHash = hash("precision");
bool bit(std::span<const std::byte, 40> mask, std::size_t index) noexcept {
    return (std::to_integer<unsigned>(mask[index / 8]) & (1U << (index % 8))) != 0;
}
const Group* group(const Catalog& c, std::uint32_t name) noexcept {
    for (std::size_t i = 0; i < c.groupCount; ++i)
        if (c.groups[i].nameHash == name) return &c.groups[i];
    return nullptr;
}
bool valid(const Catalog& c) noexcept {
    if (!c.count || c.count > c.labels.size() || !c.groupCount || c.groupCount > c.groups.size()
        || !c.rangeCount || c.rangeCount > c.ranges.size())
        return false;
    bool precision = false;
    for (std::size_t i = 0; i < c.count; ++i) {
        if (!c.labels[i] || c.labels[i] == 0xFFFFFFFFU) return false;
        precision |= c.labels[i] == precisionHash;
        for (std::size_t j = 0; j < i; ++j)
            if (c.labels[i] == c.labels[j]) return false;
    }
    if (!precision) return false;
    for (std::size_t i = 0; i < c.groupCount; ++i) {
        const auto& g = c.groups[i];
        if (!g.nameHash || g.nameHash == 0xFFFFFFFFU) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (g.nameHash == c.groups[j].nameHash) return false;
        bool any = false;
        for (std::size_t j = 0; j < 320; ++j) {
            if (!bit(g.mask, j)) continue;
            if (j >= c.count) return false;
            any = true;
        }
        if (!any) return false;
    }
    for (auto name : required)
        if (!group(c, name)) return false;
    std::array<bool, 320> covered{};
    for (std::size_t i = 0; i < c.rangeCount; ++i) {
        const auto& r = c.ranges[i];
        const auto end = std::size_t{r.start} + r.count;
        if (!r.count || end > c.count) return false;
        for (std::size_t j = r.start; j < end; ++j) {
            if (covered[j]) return false;
            covered[j] = true;
        }
    }
    return true;
}
bool read(std::span<const std::byte> b,
          std::size_t at,
          std::size_t width,
          std::uint64_t& out) noexcept {
    out = 0;
    if (at > b.size() || width > b.size() - at) return false;
    for (std::size_t i = 0; i < width; ++i)
        out |= std::uint64_t{std::to_integer<unsigned>(b[at + i])} << (i * 8U);
    return true;
}
struct Array {
    std::size_t count{}, start{}, data{}, end{};
};
bool array(std::span<const std::byte> b,
           std::size_t at,
           std::size_t capacity,
           std::uint32_t rowClass,
           std::size_t stride,
           Array& out) noexcept {
    std::uint64_t n{}, relative{}, marker{}, repeated{}, actualClass{};
    if (!read(b, at, 8, n) || !n || n > capacity || !read(b, at + 8, 8, relative)
        || at + 8 > b.size() || relative > b.size() - (at + 8))
        return false;
    const auto header = at + 8 + static_cast<std::size_t>(relative);
    if (header < 0x40 || !read(b, header - 4, 4, marker) || marker != 0x80809FBDU
        || !read(b, header, 8, repeated) || repeated != n || !read(b, header + 8, 4, actualClass)
        || actualClass != rowClass || header > b.size() || b.size() - header < 16
        || n > (b.size() - header - 16) / stride)
        return false;
    out = {static_cast<std::size_t>(n),
           header - 4,
           header + 16,
           header + 16 + static_cast<std::size_t>(n) * stride};
    return true;
}
bool mask_valid(const Catalog& c, std::span<const std::byte, 40> mask) noexcept {
    for (std::size_t i = c.count; i < 320; ++i)
        if (bit(mask, i)) return false;
    for (std::size_t i = 0; i < c.rangeCount; ++i) {
        unsigned n = 0;
        for (std::size_t j = c.ranges[i].start;
             j < std::size_t{c.ranges[i].start} + c.ranges[i].count;
             ++j)
            n += bit(mask, j);
        if (n > 1) return false;
    }
    return true;
}
unsigned intersection(const Catalog& c,
                      std::span<const std::byte, 40> mask,
                      std::uint32_t name,
                      std::size_t& last) noexcept {
    const auto* g = group(c, name);
    unsigned n = 0;
    if (g)
        for (std::size_t i = 0; i < c.count; ++i)
            if (bit(mask, i) && bit(g->mask, i)) {
                ++n;
                last = i;
            }
    return n;
}
} // namespace
bool parse(std::span<const std::byte> blob, Catalog& output) noexcept {
    output = {};
    std::uint64_t length{};
    Array labels{}, groups{}, ranges{};
    if (!read(blob, 0, 8, length) || length != blob.size()
        || !array(blob, 8, 320, 0x80800070U, 4, labels)
        || !array(blob, 24, 32, kGroupClass, 44, groups)
        || !array(blob, 40, 32, 0x808094BCU, 4, ranges))
        return false;
    const std::array arrays{labels, groups, ranges};
    for (std::size_t i = 0; i < arrays.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (arrays[i].start < arrays[j].end && arrays[j].start < arrays[i].end) return false;
    Catalog staged{};
    staged.count = labels.count;
    staged.groupCount = groups.count;
    staged.rangeCount = ranges.count;
    std::uint64_t value{};
    for (std::size_t i = 0; i < labels.count; ++i) {
        if (!read(blob, labels.data + i * 4, 4, value)) return false;
        staged.labels[i] = static_cast<std::uint32_t>(value);
    }
    for (std::size_t i = 0; i < groups.count; ++i) {
        if (!read(blob, groups.data + i * 44, 4, value)) return false;
        staged.groups[i].nameHash = static_cast<std::uint32_t>(value);
        for (std::size_t j = 0; j < 40; ++j)
            staged.groups[i].mask[j] = blob[groups.data + i * 44 + 4 + j];
    }
    for (std::size_t i = 0; i < ranges.count; ++i) {
        const auto at = ranges.data + i * 4;
        if (!read(blob, at, 2, value) || blob[at + 3] != std::byte{}) return false;
        staged.ranges[i] = {static_cast<std::uint16_t>(value),
                            static_cast<std::uint8_t>(std::to_integer<unsigned>(blob[at + 2]))};
    }
    if (!valid(staged)) return false;
    output = staged;
    return true;
}
bool replace(const Catalog& c) noexcept {
    if (!valid(c)) return false;
    const std::lock_guard lock(g_mutex);
    g_catalog = c;
    return true;
}
std::size_t count() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_catalog.count;
}
VictimRace classify_victim(std::span<const std::byte, 40> victim) noexcept {
    const std::lock_guard lock(g_mutex);
    const auto& c = g_catalog;
    std::size_t marker = c.count;
    for (std::size_t i = 0; i < c.count; ++i) {
        if (c.labels[i] == hash("forsaken")) {
            marker = i;
            break;
        }
    }
    // Do not change classification of old, unmarked victims, even with unavailable labels.
    if (marker == c.count || !bit(victim, marker)) return VictimRace::unknown;
    bool hasModifierRange = false;
    for (std::size_t i = 0; i < c.rangeCount; ++i) {
        const auto& range = c.ranges[i];
        hasModifierRange |=
            marker >= range.start && marker < std::size_t{range.start} + range.count;
    }
    // In authored 80C70CA1 this is range250..256: forsaken is exclusive with taken,
    // human, awoken, exo, ghost and one unresolved label. Resolve its position dynamically.
    std::size_t unused{};
    if (!hasModifierRange || !mask_valid(c, victim)
        || intersection(c, victim, hash("combatant"), unused) != 1
        || intersection(c, victim, hash("player"), unused) != 0)
        return VictimRace::invalid;
    return VictimRace::scorn;
}
std::optional<std::uint32_t> classify_victim_rank(std::span<const std::byte, 40> victim) noexcept {
    const std::lock_guard lock(g_mutex);
    const auto& c = g_catalog;
    std::size_t unused{};
    if (!c.count || !mask_valid(c, victim)
        || intersection(c, victim, hash("combatant"), unused) != 1
        || intersection(c, victim, hash("player"), unused) != 0)
        return {};
    constexpr std::array names{
        hash("base"), hash("boss"), hash("elite"), hash("megaboss"), hash("miniboss")};
    // Require the reviewed five-name exclusive range, but never hardcode bit positions/order.
    for (std::size_t i = 0; i < c.rangeCount; ++i) {
        const auto& range = c.ranges[i];
        if (range.count != names.size()) continue;
        unsigned recognized = 0;
        std::optional<std::uint32_t> selected;
        for (std::size_t j = range.start; j < std::size_t{range.start} + range.count; ++j) {
            for (const auto name : names)
                if (c.labels[j] == name) {
                    ++recognized;
                    if (bit(victim, j)) selected = name;
                    break;
                }
        }
        if (recognized == names.size()) return selected;
    }
    return {};
}
Source classify(std::span<const std::byte, 40> source,
                std::span<const std::byte, 40> actor) noexcept {
    const std::lock_guard lock(g_mutex);
    const auto& c = g_catalog;
    if (!c.count || !mask_valid(c, source) || !mask_valid(c, actor)) return {};
    std::size_t player{}, actorPlayer{}, unused{}, weapon{};
    if (intersection(c, source, hash("player"), player) != 1
        || intersection(c, actor, hash("player"), actorPlayer) != 1 || player != actorPlayer
        || intersection(c, source, hash("combatant"), unused))
        return {};
    Source out{};
    out.playerClassHash = c.labels[player];
    out.melee = intersection(c, source, hash("melee"), unused) != 0;
    std::size_t grenade{}, superAbility{};
    const auto grenadeCount = intersection(c, source, hash("grenade"), grenade);
    const auto superCount = intersection(c, source, hash("super"), superAbility);
    out.grenade = grenadeCount != 0;
    out.superAbility = superCount != 0;
    const bool ability = intersection(c, source, hash("ability"), unused) != 0;
    out.ability = ability && intersection(c, source, hash("weapon"), unused) == 0;
    out.weapon = intersection(c, source, hash("weapon"), weapon) == 1 && !ability && !out.melee
                 && !out.grenade && !out.superAbility;
    if (out.weapon) out.weaponClass = c.labels[weapon];
    // Keep existing generic predicates, but never name an ambiguous/mixed ability source.
    if (grenadeCount + superCount == 1 && !out.melee
        && intersection(c, source, hash("weapon"), unused) == 0) {
        out.abilityLabelHash = c.labels[grenadeCount ? grenade : superAbility];
    }
    if (!out.weapon && !out.melee && !out.grenade && !out.superAbility && !out.ability) return {};
    for (std::size_t i = 0; i < c.count; ++i)
        if (c.labels[i] == precisionHash) out.precision = bit(source, i);
    out.attributed = true;
    return out;
}
bool mask_for_labels(std::span<const std::uint32_t> labels,
                     std::array<std::byte, 40>& output) noexcept {
    output = {};
    const std::lock_guard lock(g_mutex);
    if (g_catalog.count == 0) return false;
    for (const auto label : labels) {
        std::size_t index = 0;
        while (index < g_catalog.count && g_catalog.labels[index] != label)
            ++index;
        if (index == g_catalog.count) return false;
        const auto bit = std::byte{static_cast<unsigned char>(1U << (index % 8))};
        if ((output[index / 8] & bit) != std::byte{}) return false;
        output[index / 8] |= bit;
    }
    return true;
}
} // namespace sunrise::state::build_data::combat_labels
