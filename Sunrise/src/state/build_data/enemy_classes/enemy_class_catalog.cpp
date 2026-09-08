#include "enemy_class_catalog.h"

#include <mutex>

#include "definition.h"

namespace sunrise::state::build_data::enemy_classes {
namespace {
std::mutex g_mutex;
Catalog g_catalog{};
} // namespace
bool replace(const Catalog& catalog) noexcept {
    if (catalog.count == 0 || catalog.count > catalog.rows.size()) return false;
    for (std::size_t i = 0; i < catalog.count; ++i) {
        if (catalog.rows[i].classHash == 0 || catalog.rows[i].classHash == 0x811C9DC5U)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (catalog.rows[i].classHash == catalog.rows[j].classHash) return false;
    }
    const std::lock_guard lock(g_mutex);
    g_catalog = catalog;
    return true;
}
std::size_t count() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_catalog.count;
}
Lookup classify(std::int16_t index, std::uint32_t classHash, std::uint32_t& race) noexcept {
    race = 0;
    const std::lock_guard lock(g_mutex);
    if (g_catalog.count == 0) return Lookup::unavailable;
    if (index < -1 || (index >= 0 && static_cast<std::size_t>(index) >= g_catalog.count))
        return Lookup::invalid;
    const auto* indexed = index >= 0 ? &g_catalog.rows[static_cast<std::size_t>(index)] : nullptr;
    const decltype(indexed) direct = [&]() -> decltype(indexed) {
        for (std::size_t i = 0; i < g_catalog.count; ++i)
            if (g_catalog.rows[i].classHash == classHash) return &g_catalog.rows[i];
        return static_cast<decltype(indexed)>(nullptr);
    }();
    if (indexed && direct && indexed->raceHash != direct->raceHash) return Lookup::invalid;
    const auto* row = indexed ? indexed : direct;
    if (!row) return Lookup::unknown;
    if (row->raceHash == 0) return Lookup::nonEnemy;
    race = row->raceHash;
    return Lookup::enemy;
}
} // namespace sunrise::state::build_data::enemy_classes
