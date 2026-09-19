#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../../middleware/content/packages/items/item_presentation_reader.h"
#include "../../../state/build_data/items/item_catalog.h"

namespace sunrise::client::ui::items {
namespace package = middleware::content::packages::items;

enum class GrantPolicy : std::uint8_t { unknown, legitimate, typeless };
// Display grouping only. A category never authorizes an item grant.
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
    typeless,
    other
};
inline constexpr std::array<const char*, 11> kCategoryNames{"All",
                                                            "Bounties",
                                                            "Quests",
                                                            "Engrams",
                                                            "Weapons",
                                                            "Armor",
                                                            "Consumables",
                                                            "Materials",
                                                            "Cosmetics",
                                                            "Typeless",
                                                            "Other"};
struct Objective {
    std::uint16_t index{};
    std::uint32_t hash{};
    std::int32_t completion{};
    bool itemProgress{};
    bool resolved{};
    std::string description{};
};
struct Entry {
    state::build_data::items::Definition identity{};
    std::uint16_t iconIndex{package::kNoIcon};
    GrantPolicy policy{};
    Category category{Category::other};
    bool detailReady{};
    bool bounty{};
    std::int32_t quantityLimit{1};
    std::string name{}, description{}, itemType{}, search{};
    std::vector<Objective> objectives{};
};
struct Catalog {
    std::vector<Entry> entries{};
    std::size_t names{}, descriptions{}, classified{};
    bool packagesReady{};
};
struct IconResult {
    std::uint64_t request{};
    package::Icon icon{};
    bool available{};
};

namespace catalog {
/** Starts one cancellable worker after State publishes item identities. No I/O on the UI thread. */
void start() noexcept;
void stop() noexcept;
void reload() noexcept;
[[nodiscard]] std::shared_ptr<const Catalog> snapshot() noexcept;
[[nodiscard]] const char* status() noexcept;
/** Combined queued and completed work is capped at sixteen icons. */
[[nodiscard]] bool request_icon(std::uint64_t request, std::uint16_t index) noexcept;
[[nodiscard]] bool take_icon(IconResult& output) noexcept;
} // namespace catalog
} // namespace sunrise::client::ui::items
