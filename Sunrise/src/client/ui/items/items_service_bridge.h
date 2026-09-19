#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "items_catalog.h"

namespace sunrise::client::ui::items::service {
struct Feedback {
    bool accepted{};
    /** Held pursuits expected once every queued acquisition has been published, or zero. */
    std::size_t expected{};
    std::array<char, 384> text{};
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
[[nodiscard]] Feedback grant(const Entry& entry, std::int32_t quantity) noexcept;
[[nodiscard]] Feedback set_lane(std::uint64_t instance,
                                std::uint16_t item,
                                std::uint8_t lane,
                                std::int32_t value) noexcept;
[[nodiscard]] Feedback complete_bounties() noexcept;
/** Grants or reuses one installed bounty and completes it, ready to redeem at its vendor. */
[[nodiscard]] Feedback page_bounty(const Entry& entry) noexcept;

/** Installed bounties, in definition order, split into fixed pages. */
struct Pages {
    std::size_t bounties{};
    std::size_t count{};
};
inline constexpr std::size_t kBountyPageSize = 40;
[[nodiscard]] Pages bounty_pages() noexcept;

/**
 * Discards every held bounty, then queues one page of installed bounties for acquisition.
 * Each lands through the normal acquisition publication, so the caller completes them once
 * `expected` of them are held rather than acting on instances that do not exist yet.
 * @param page One-based page over the installed bounty order.
 * @return Counts for the page, or a refusal when the page is outside the installed range.
 */
[[nodiscard]] Feedback grant_bounty_page(std::size_t page) noexcept;
[[nodiscard]] Feedback clear(Clear category) noexcept;
} // namespace sunrise::client::ui::items::service
