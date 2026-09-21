#pragma once

#include <cstdint>
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
[[nodiscard]] Inventory inventory() noexcept;
[[nodiscard]] bool remove(std::uint64_t character, const HeldItem& item) noexcept;
[[nodiscard]] bool
set_quantity(std::uint64_t character, const HeldItem& item, std::int32_t quantity) noexcept;
[[nodiscard]] bool grantable(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
[[nodiscard]] bool grant(std::uint16_t itemIndex, std::uint32_t quantity) noexcept;
} // namespace sunrise::server::ui::items
