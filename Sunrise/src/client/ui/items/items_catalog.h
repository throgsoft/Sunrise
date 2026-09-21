#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../../state/build_data/items/item_catalog.h"
#include "items_presentation_reader.h"

namespace sunrise::client::ui::items {
namespace package = presentation;

enum class Category : std::uint8_t {
    all,
    bounties,
    quests,
    engrams,
    weapons,
    armor,
    consumables,
    materials,
    cosmetics,
    other
};
inline constexpr std::array<const char*, 10> kCategoryNames{"All",
                                                            "Bounties",
                                                            "Quests",
                                                            "Engrams",
                                                            "Weapons",
                                                            "Armor",
                                                            "Consumables",
                                                            "Materials",
                                                            "Cosmetics",
                                                            "Other"};
struct Entry {
    state::build_data::items::Definition identity{};
    std::uint16_t iconIndex{package::kNoIcon};
    Category category{Category::other};
    std::string name{}, description{}, itemType{}, search{};
};
struct Catalog {
    std::vector<Entry> entries{};
    std::size_t names{};
};
struct IconResult {
    std::uint64_t request{};
    package::Icon icon{};
    bool available{};
};

namespace catalog {
/** Starts the package worker after State publishes item identities. */
void start() noexcept;
void stop() noexcept;
[[nodiscard]] std::shared_ptr<const Catalog> snapshot() noexcept;
[[nodiscard]] const char* status() noexcept;
/** Combined queued and completed work is capped at sixteen icons. */
[[nodiscard]] bool request_icon(std::uint64_t request, std::uint16_t index) noexcept;
[[nodiscard]] bool take_icon(IconResult& output) noexcept;
} // namespace catalog
} // namespace sunrise::client::ui::items
