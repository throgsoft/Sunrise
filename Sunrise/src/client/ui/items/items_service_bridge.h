#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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
    /** The Client orders a bucket's grid by this, so the module lists held pursuits by it too. */
    std::int32_t mutationSerial{};
    /** Every declared objective has reached its value, so the pursuit is ready to redeem. */
    bool complete{};
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
/**
 * Asks the reward policy itself whether it would place this item, committing nothing.
 * Classification mirrors the policy's placements to keep the catalog cheap; this is the policy,
 * so the selected item reports what a grant would really do. It reads the account, so the
 * answer changes with capacity and with pursuits already held.
 */
[[nodiscard]] Feedback grantable(const Entry& entry) noexcept;

[[nodiscard]] Feedback set_lane(std::uint64_t instance,
                                std::uint16_t item,
                                std::uint8_t lane,
                                std::int32_t value) noexcept;
[[nodiscard]] Feedback complete_bounties() noexcept;

/** Installed bounties, in definition order, split into fixed pages. */
struct Pages {
    std::size_t bounties{};
    std::size_t count{};
};
inline constexpr std::size_t kBountyPageSize = 40;
[[nodiscard]] Pages bounty_pages() noexcept;

/**
 * Installed bounty definition indices on one page, in installed order.
 * Proving and saving one acquisition copies an account image, so a caller feeds these in over
 * several frames rather than holding the render thread for a whole page.
 * @param page One-based page over the installed bounty order.
 * @return The page's definition indices, empty when the page is outside the installed range.
 */
[[nodiscard]] std::vector<std::uint16_t> bounty_page(std::size_t page) noexcept;

/** Queues one page of installed bounties for the normal acquisition publication. */
[[nodiscard]] bool queue_bounty_page(std::span<const std::uint16_t> indices) noexcept;

[[nodiscard]] Feedback clear(Clear category) noexcept;
} // namespace sunrise::client::ui::items::service
