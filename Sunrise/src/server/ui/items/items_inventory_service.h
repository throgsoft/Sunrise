#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::server::ui::items {
struct Bucket {
    std::uint8_t id{};
    std::uint8_t scope{};
    std::uint16_t capacity{};
};
struct HeldItem {
    std::uint64_t instance{};
    std::uint32_t hash{};
    std::uint16_t index{};
    std::uint8_t bucket{};
    std::int32_t quantity{};
    std::int32_t serial{};
    bool equipped{};
};
struct Inventory {
    std::uint64_t character{};
    std::vector<Bucket> buckets;
    std::vector<HeldItem> items;
};
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
[[nodiscard]] Inventory inventory() noexcept;
[[nodiscard]] bool remove(std::uint64_t character, const HeldItem& item) noexcept;
[[nodiscard]] bool
set_quantity(std::uint64_t character, const HeldItem& item, std::int32_t quantity) noexcept;
[[nodiscard]] bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
[[nodiscard]] bool grant(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
[[nodiscard]] SeasonPass season_pass(std::uint16_t rewardIndex) noexcept;
[[nodiscard]] bool grant_season_pass_reward(std::span<const std::byte> request) noexcept;
[[nodiscard]] bool unclaim_season_pass_reward(std::uint16_t rewardIndex) noexcept;
[[nodiscard]] bool set_seasonal_experience(std::int32_t expected, std::int32_t experience) noexcept;
} // namespace sunrise::server::ui::items
