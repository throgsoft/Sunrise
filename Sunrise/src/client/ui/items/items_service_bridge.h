#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "items_catalog.h"

namespace sunrise::client::ui::items::service {
struct Feedback {
    bool accepted{};
    std::array<char, 384> text{};
    std::uint64_t requestId{};
    bool pending{};
};
struct Held {
    std::uint64_t instance{};
    std::uint32_t hash{};
    std::uint16_t index{};
    std::array<std::int32_t, 8> values{};
    bool editable{};
    const char* reason{};
};
struct Inventory {
    std::uint64_t character{};
    std::vector<Held> bounties{};
    std::size_t unresolved{};
    bool ready{};
};
enum class Clear : std::uint8_t { weapons, armor, bounties, engrams, seasonPass };
[[nodiscard]] Inventory inventory() noexcept;
[[nodiscard]] GrantPolicy classify(const Entry& entry) noexcept;
/** Optional installed bright-source explanation for the item details panel. */
[[nodiscard]] const char* grant_variant(const Entry& entry) noexcept;
[[nodiscard]] Feedback grant(const Entry& entry, std::int32_t quantity) noexcept;
/** Poll a queued grant without entering the BAP transport or touching SQLite. */
[[nodiscard]] Feedback grant_receipt(std::uint64_t requestId) noexcept;
[[nodiscard]] Feedback set_lane(std::uint64_t instance, std::uint16_t item,
                                 std::uint8_t lane, std::int32_t value) noexcept;
[[nodiscard]] Feedback complete_bounties() noexcept;
[[nodiscard]] Feedback clear(Clear category) noexcept;
} // namespace sunrise::client::ui::items::service
