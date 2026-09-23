#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sunrise::server::ui::items {
/** Installed capacity and ownership of an inventory bucket. */
struct Bucket {
    std::uint8_t id{};
    std::uint8_t scope{};
    std::uint16_t capacity{};
};
/** Inventory identity and expected serial used to reject stale edits. */
struct HeldItem {
    std::uint64_t instance{};
    std::uint32_t hash{};
    std::uint16_t index{};
    std::uint8_t bucket{};
    std::int32_t quantity{};
    std::int32_t serial{};
    bool equipped{};
};
/** Snapshot of the selected character and profile inventory. */
struct Inventory {
    std::uint64_t character{};
    std::vector<Bucket> buckets;
    std::vector<HeldItem> items;
};
/** Saved pass state; grantable is an availability hint, not a prepared grant. */
struct SeasonPass {
    std::int32_t experience{};
    std::uint16_t rank{};
    bool available{};
    bool claimed{};
    bool acquired{};
    bool progressFlag{};
    bool pending{};
    bool grantable{};
};
/** Reads inventory for the currently selected character. */
[[nodiscard]] Inventory inventory() noexcept;
/** Removes the observed stack only if its saved identity and quantity still match. */
[[nodiscard]] bool remove(std::uint64_t character, const HeldItem& item) noexcept;
/** Replaces the observed stack quantity; zero removes it. */
[[nodiscard]] bool
set_quantity(std::uint64_t character, const HeldItem& item, std::int32_t quantity) noexcept;
/** Checks the installed route and quantity bounds without resolving a reward draw. */
[[nodiscard]] bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
/** Queues an acquisition through the public server reward path. */
[[nodiscard]] bool grant(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
/** Reads claim, acquisition and pending state without preparing a reward. */
[[nodiscard]] SeasonPass season_pass(std::uint16_t rewardIndex) noexcept;
/** Queues a manual claim; the server rechecks eligibility before committing. */
[[nodiscard]] bool grant_season_pass_reward(std::uint16_t rewardIndex) noexcept;
/** Resets a manual claim flag without removing its previously granted inventory. */
[[nodiscard]] bool unclaim_season_pass_reward(std::uint16_t rewardIndex) noexcept;
/** Replaces XP only if the observed value still matches, then refreshes progression. */
[[nodiscard]] bool set_seasonal_experience(std::int32_t expected, std::int32_t experience) noexcept;
} // namespace sunrise::server::ui::items
