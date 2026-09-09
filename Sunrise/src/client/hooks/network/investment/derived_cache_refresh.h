#pragma once
#include <array>
#include <cstdint>

namespace sunrise::client::hooks::network::investment::derived_refresh {
enum class Result { unchanged, invalidated, failed, full };

/** One received revision must reach every native cache, not only the first accessor.
 * Caller serializes access. Failed writes never consume a revision. No eviction churn. */
struct Ledger {
    struct Entry {
        std::uintptr_t cache{};
        std::uint64_t revision{};
    };
    std::array<Entry, 64> entries{};

    template <class Invalidate>
    Result apply(std::uintptr_t cache, std::uint64_t revision, Invalidate&& invalidate) noexcept {
        if (!cache || !revision) return Result::unchanged;
        Entry* available = nullptr;
        for (auto& row : entries) {
            if (row.cache == cache) {
                available = &row;
                break;
            }
            if (!row.cache && !available) available = &row;
        }
        if (!available) return Result::full;
        if (available->cache == cache && available->revision >= revision) return Result::unchanged;
        if (!invalidate(cache)) return Result::failed;
        *available = {cache, revision};
        return Result::invalidated;
    }
};
} // namespace sunrise::client::hooks::network::investment::derived_refresh
