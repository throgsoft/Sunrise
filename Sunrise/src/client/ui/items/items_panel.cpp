#include "items_panel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

#include "../../../state/build_data/runtime.h"
#include "../../hooks/graphics/renderer/state.h"
#include "items_catalog.h"
#include "items_icon_cache.h"
#include "items_service_bridge.h"

namespace sunrise::client::ui::items {
namespace {
std::array<char, 256> g_search{};
std::string g_appliedSearch;
std::shared_ptr<const Catalog> g_filteredCatalog;
std::vector<std::size_t> g_filtered;
int g_selected{-1};
int g_quantity{1};
service::Feedback g_feedback{};
service::Feedback g_grantFeedback{};
service::Inventory g_inventory{};
double g_refreshAt{};
std::uint64_t g_heldSelection{};
std::array<int, 7> g_laneValues{};
std::array<bool, 7> g_laneEdited{};
int g_clearCategory{-1};
int g_bountyPage{1};
int g_heldPage{1};
int g_probed{-1};
service::Feedback g_probe{};
std::size_t g_pageExpected{};
double g_pageDeadline{};
std::uint64_t g_clearCharacter{};
enum class Sort : int { type, name, id };
constexpr std::array<const char*, 3> kSortNames{"Type", "Name", "ID"};
int g_category{static_cast<int>(Category::all)}, g_appliedCategory{-1};
int g_sort{static_cast<int>(Sort::type)}, g_appliedSort{-1};
bool g_resetGridScroll{};

constexpr std::array<const char*, 5> kClearNames{
    "Weapons", "Armor", "Bounties", "Engrams", "Season pass"};
constexpr std::array<const char*, 5> kClearDescriptions{
    "Remove unequipped kinetic, energy and heavy weapons from the selected character.",
    "Remove unequipped helmet, gauntlets, chest, legs and class armor from the selected character.",
    "Remove held bounties without rewards. Quests are preserved.",
    "Remove held engrams from the selected character without decrypting them.",
    ("Reset account season pass XP and mapped reward claims for all classes. Already granted items "
     "remain.")};

void select_held(const service::Held& held) {
    g_heldSelection = held.instance;
    std::copy_n(held.values.begin() + 1, 7, g_laneValues.begin());
    g_laneEdited.fill(false);
}
void refresh() {
    g_probed = -1;
    const auto previous = g_inventory.character;
    g_inventory = service::inventory();
    g_refreshAt = ImGui::GetTime() + 1.0;
    if (previous != g_inventory.character) {
        g_heldSelection = 0;
        g_clearCategory = -1;
    }
    for (const auto& held : g_inventory.bounties) {
        if (held.instance != g_heldSelection) {
            continue;
        }
        for (std::size_t lane = 0; lane < g_laneValues.size(); ++lane) {
            if (!g_laneEdited[lane]) {
                g_laneValues[lane] = held.values[lane + 1];
            }
        }
        return;
    }
    g_heldSelection = 0;
    g_laneEdited.fill(false);
}
void feedback(service::Feedback result) {
    g_feedback = result;
    refresh();
}
void grant_feedback(service::Feedback result) {
    g_grantFeedback = result;
    g_feedback = {};
    refresh();
}
const Entry* find(const Catalog& catalog, std::uint16_t index, std::uint32_t hash) noexcept {
    const auto found = std::lower_bound(catalog.entries.begin(),
                                        catalog.entries.end(),
                                        index,
                                        [](const Entry& entry, std::uint16_t value) {
                                            return entry.identity.definitionIndex < value;
                                        });
    return found != catalog.entries.end() && found->identity.definitionIndex == index
                   && found->identity.definitionHash == hash
               ? &*found
               : nullptr;
}
const char* name(const Entry& entry) noexcept {
    return entry.name.empty() ? "Unnamed item" : entry.name.c_str();
}
void icon(const Entry& entry, float side) {
    const auto texture = icons::get(entry.iconIndex);
    if (texture != ImTextureID_Invalid) {
        ImGui::Image(ImTextureRef(texture), {side, side});
    } else {
        ImGui::BeginGroup();
        ImGui::TextDisabled("Icon unavailable");
        ImGui::Dummy({side, (std::max)(0.0f, side - ImGui::GetTextLineHeightWithSpacing())});
        ImGui::EndGroup();
    }
}
void objectives(const Entry& entry) {
    for (std::size_t i = 0; i < entry.objectives.size(); ++i) {
        const auto& objective = entry.objectives[i];
        ImGui::Text("Lane %zu: objective %u / 0x%08X",
                    i + 1,
                    static_cast<unsigned>(objective.index),
                    objective.hash);
        if (!objective.description.empty()) {
            ImGui::TextWrapped("%s", objective.description.c_str());
        }
        if (objective.resolved) {
            ImGui::TextDisabled("Completion: %d%s",
                                objective.completion,
                                objective.itemProgress ? "" : " (shared / unsupported source)");
        } else {
            ImGui::TextDisabled("Objective metadata unavailable.");
        }
    }
}
bool matches(std::string_view haystack, std::string_view query) noexcept {
    while (!query.empty()) {
        const auto first = query.find_first_not_of(" \t");
        if (first == std::string_view::npos) {
            return true;
        }
        query.remove_prefix(first);
        const auto end = query.find_first_of(" \t");
        const auto word = query.substr(0, end);
        if (haystack.find(word) == std::string_view::npos) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        query.remove_prefix(end);
    }
    return true;
}
int compare_text(std::string_view left, std::string_view right) noexcept {
    // Literal English package strings: fold ASCII without allocating comparator temporaries.
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    for (std::size_t i = 0, count = (std::min)(left.size(), right.size()); i < count; ++i) {
        const auto a = fold(static_cast<unsigned char>(left[i]));
        const auto b = fold(static_cast<unsigned char>(right[i]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
}
int compare_optional_text(std::string_view left, std::string_view right) noexcept {
    if (left.empty() != right.empty()) {
        return left.empty() ? 1 : -1;
    }
    return compare_text(left, right);
}
void prepare_filter(const std::shared_ptr<const Catalog>& data) {
    std::string query(g_search.data());
    std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (g_filteredCatalog != data || query != g_appliedSearch || g_category != g_appliedCategory
        || g_sort != g_appliedSort) {
        g_filtered.clear();
        for (std::size_t i = 0; i < data->entries.size(); ++i) {
            if ((g_category == static_cast<int>(Category::all)
                 || static_cast<int>(data->entries[i].category) == g_category)
                && matches(data->entries[i].search, query)) {
                g_filtered.push_back(i);
            }
        }
        // Sort the view indices only: selected items, held instances and ID lookup stay stable.
        std::sort(g_filtered.begin(), g_filtered.end(), [&](std::size_t left, std::size_t right) {
            const auto& a = data->entries[left];
            const auto& b = data->entries[right];
            if (g_sort == static_cast<int>(Sort::type)) {
                if (a.category != b.category) {
                    return a.category < b.category;
                }
                const int type = compare_optional_text(a.itemType, b.itemType);
                if (type != 0) {
                    return type < 0;
                }
            }
            if (g_sort != static_cast<int>(Sort::id)) {
                const int name = compare_optional_text(a.name, b.name);
                if (name != 0) {
                    return name < 0;
                }
            }
            if (a.identity.definitionIndex != b.identity.definitionIndex) {
                return a.identity.definitionIndex < b.identity.definitionIndex;
            }
            return left < right;
        });
        g_filteredCatalog = data;
        g_appliedSearch = std::move(query);
        g_appliedCategory = g_category;
        g_appliedSort = g_sort;
        g_resetGridScroll = true;
    }
}

void bounty_lanes(const Entry& entry, const service::Held& held) {
    ImGui::TextDisabled("Instance 0x%016llX", static_cast<unsigned long long>(held.instance));
    ImGui::TextDisabled("Saved expiry: %d", held.values[0]);
    if (!held.editable) {
        ImGui::TextWrapped("%s", held.reason);
    }
    for (std::size_t i = 0; i < entry.objectives.size() && i < g_laneValues.size(); ++i) {
        const auto& objective = entry.objectives[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Text("Lane %zu: %d / %d", i + 1, held.values[i + 1], objective.completion);
        if (!objective.description.empty()) {
            ImGui::TextWrapped("%s", objective.description.c_str());
        }
        ImGui::TextDisabled(
            "Objective %u / 0x%08X", static_cast<unsigned>(objective.index), objective.hash);
        ImGui::BeginDisabled(!held.editable || !objective.resolved || !objective.itemProgress);
        ImGui::SetNextItemWidth((std::min)(145.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputInt("##value", &g_laneValues[i], 0, 0)) {
            g_laneEdited[i] = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("/ %d", objective.completion);
        if (ImGui::GetContentRegionAvail().x >= 230) {
            ImGui::SameLine();
        }
        const bool apply = ImGui::Button("Set lane");
        ImGui::EndDisabled();
        if (!objective.resolved) {
            ImGui::TextDisabled("Objective metadata unavailable.");
        } else if (!objective.itemProgress) {
            ImGui::TextDisabled("Shared / unsupported source; lane editing disabled.");
        }
        ImGui::PopID();
        if (apply) {
            const auto result = service::set_lane(
                held.instance, held.index, static_cast<std::uint8_t>(i + 1), g_laneValues[i]);
            if (result.accepted) {
                g_laneEdited[i] = false;
            }
            // Refresh replaces the vector behind held; leave the editor immediately afterward.
            feedback(result);
            return;
        }
    }
}

void catalog_bounty(const Entry& entry) {
    ImGui::SeparatorText("Held bounty");
    if (!g_inventory.ready) {
        ImGui::TextDisabled("Select a character to edit held objectives.");
        return;
    }
    const service::Held* selected = nullptr;
    std::size_t count = 0;
    for (const auto& held : g_inventory.bounties) {
        if (held.index != entry.identity.definitionIndex
            || held.hash != entry.identity.definitionHash) {
            continue;
        }
        ++count;
        if (!selected || held.instance == g_heldSelection) {
            selected = &held;
        }
    }
    if (!selected) {
        ImGui::TextWrapped("This character is not holding this bounty.");
        objectives(entry);
        return;
    }
    if (selected->instance != g_heldSelection) {
        select_held(*selected);
    }
    if (count > 1) {
        char preview[64]{};
        std::snprintf(preview,
                      sizeof preview,
                      "Instance 0x%016llX",
                      static_cast<unsigned long long>(selected->instance));
        ImGui::TextDisabled("%zu held copies; choose the instance to edit.", count);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##held_instance", preview)) {
            for (const auto& held : g_inventory.bounties) {
                if (held.index != entry.identity.definitionIndex
                    || held.hash != entry.identity.definitionHash) {
                    continue;
                }
                char label[80]{};
                std::snprintf(label,
                              sizeof label,
                              "0x%016llX%s",
                              static_cast<unsigned long long>(held.instance),
                              held.editable ? "" : " (read-only)");
                if (ImGui::Selectable(label, held.instance == g_heldSelection)) {
                    select_held(held);
                    selected = &held;
                }
                if (held.instance == g_heldSelection) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    bounty_lanes(entry, *selected);
}

void catalog_grid(const Catalog& data, float requestedHeight, float requestedWidth) {
    const auto& style = ImGui::GetStyle();
    const float fontSize = ImGui::GetFontSize();
    const float padding = 8;
    const float iconSide = (std::clamp)(fontSize * 3, 42.0f, 58.0f);
    const float tileHeight = padding * 2 + iconSide + style.ItemInnerSpacing.y + fontSize * 2;
    const float rowPitch = tileHeight + style.ItemSpacing.y;
    const float outerWidth =
        requestedWidth > 0 ? requestedWidth : (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
    const float innerWidth = (std::max)(1.0f,
                                        outerWidth - style.WindowPadding.x * 2 - style.ScrollbarSize
                                            - style.FramePadding.x);
    const float minimumWidth = iconSide + padding * 2 + 14;
    const int columns = (std::clamp)(static_cast<int>((innerWidth + style.ItemSpacing.x)
                                                      / (minimumWidth + style.ItemSpacing.x)),
                                     1,
                                     12);
    // A tile with no free cache slot draws no icon at all, so the visible rows are bounded by the
    // icon budget rather than by a fixed count. The reserve covers the detail and held-bounty art.
    constexpr int kIconBudget = 58;
    const int maximumRows = (std::max)(2, kIconBudget / columns);
    const float height =
        (std::min)(requestedHeight,
                   rowPitch * static_cast<float>(maximumRows) + style.WindowPadding.y * 2);
    if (ImGui::BeginChild("##item_grid",
                          {requestedWidth, height},
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        if (g_resetGridScroll) {
            ImGui::SetScrollY(0);
            g_resetGridScroll = false;
        }
        const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
        const float tileWidth =
            (std::max)(1.0f,
                       (width - style.ItemSpacing.x * static_cast<float>(columns - 1))
                           / static_cast<float>(columns));
        if (g_filtered.empty()) {
            ImGui::TextWrapped("No matching items. Try a name, ID, hash or objective.");
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>((g_filtered.size() + columns - 1) / columns), rowPitch);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const auto offset = static_cast<std::size_t>(row) * columns + column;
                    if (offset >= g_filtered.size()) {
                        break;
                    }
                    if (column) {
                        ImGui::SameLine(0, style.ItemSpacing.x);
                    }
                    const auto position = g_filtered[offset];
                    const auto& entry = data.entries[position];
                    ImGui::PushID(static_cast<int>(position));
                    ImGui::PushStyleColor(
                        ImGuiCol_Button,
                        style.Colors[g_selected == static_cast<int>(position) ? ImGuiCol_Header
                                                                              : ImGuiCol_FrameBg]);
                    if (ImGui::Button("##item", {tileWidth, tileHeight})) {
                        g_selected = static_cast<int>(position);
                        g_quantity = 1;
                    }
                    ImGui::PopStyleColor();
                    const bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemVisible()) {
                        const auto top = ImGui::GetItemRectMin();
                        const auto bottom = ImGui::GetItemRectMax();
                        auto* draw = ImGui::GetWindowDrawList();
                        draw->PushClipRect(top, bottom, true);
                        const float side =
                            (std::max)(1.0f, (std::min)(iconSide, tileWidth - padding * 2));
                        const ImVec2 imageMin{top.x + (tileWidth - side) * 0.5f, top.y + padding};
                        const ImVec2 imageMax{imageMin.x + side, imageMin.y + side};
                        const auto texture = icons::get(entry.iconIndex);
                        if (texture != ImTextureID_Invalid) {
                            draw->AddImage(ImTextureRef(texture), imageMin, imageMax);
                        } else {
                            constexpr const char* missing = "Icon\nunavailable";
                            const auto size = ImGui::CalcTextSize(missing);
                            draw->AddText({top.x + (tileWidth - size.x) * 0.5f,
                                           imageMin.y + (iconSide - size.y) * 0.5f},
                                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                          missing);
                        }
                        const ImVec2 textMin{top.x + padding,
                                             top.y + padding + iconSide + style.ItemInnerSpacing.y};
                        const ImVec4 textClip{
                            textMin.x, textMin.y, bottom.x - padding, bottom.y - padding};
                        draw->AddText(ImGui::GetFont(),
                                      fontSize,
                                      textMin,
                                      ImGui::GetColorU32(ImGuiCol_Text),
                                      name(entry),
                                      nullptr,
                                      (std::max)(1.0f, tileWidth - padding * 2),
                                      &textClip);
                        draw->PopClipRect();
                        if (g_selected == static_cast<int>(position)) {
                            draw->AddRect(top,
                                          bottom,
                                          ImGui::GetColorU32(ImGuiCol_CheckMark),
                                          style.FrameRounding);
                        }
                    }
                    if (hovered) {
                        ImGui::SetTooltip("%s", name(entry));
                    }
                    ImGui::PopID();
                }
            }
        }
    }
    ImGui::EndChild();
}
void selected_item(const std::shared_ptr<const Catalog>& data);

void catalog_tab(const std::shared_ptr<const Catalog>& data) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##item_search",
                             "Search id, hash, name, description or objective",
                             g_search.data(),
                             g_search.size());
    const bool wideFilters = ImGui::GetContentRegionAvail().x >= 440;
    ImGui::SetNextItemWidth(wideFilters ? 180.0f
                                        : (std::max)(1.0f,
                                                     ImGui::GetContentRegionAvail().x
                                                         - ImGui::CalcTextSize("Category").x
                                                         - ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::Combo(
        "Category", &g_category, kCategoryNames.data(), static_cast<int>(kCategoryNames.size()));
    if (wideFilters) {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(wideFilters ? 110.0f
                                        : (std::max)(1.0f,
                                                     ImGui::GetContentRegionAvail().x
                                                         - ImGui::CalcTextSize("Sort").x
                                                         - ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::Combo("Sort", &g_sort, kSortNames.data(), static_cast<int>(kSortNames.size()));
    if (wideFilters) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu matches / %zu installed / %zu grantable",
                            g_filtered.size(),
                            data->entries.size(),
                            data->classified);
    }
    if (g_category == static_cast<int>(Category::dummies)) {
        ImGui::TextWrapped("Only identified dummies are listed here. Items this build cannot "
                           "classify remain read-only.");
    }
    if (!wideFilters) {
        ImGui::TextDisabled("%zu matches / %zu installed / %zu grantable",
                            g_filtered.size(),
                            data->entries.size(),
                            data->classified);
    }
    const auto& style = ImGui::GetStyle();
    const float available = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
    // Side by side once there is room for both; the grid keeps the whole column height either way.
    const bool sideBySide = available >= 640.0f;
    const float detailWidth =
        sideBySide ? (std::clamp)(available * 0.34f, 260.0f, 380.0f) : available;
    const float gridWidth =
        sideBySide ? (std::max)(160.0f, available - detailWidth - style.ItemSpacing.x) : 0.0f;
    const float gridHeight =
        sideBySide ? (std::max)(160.0f, ImGui::GetContentRegionAvail().y)
                   : (std::clamp)(ImGui::GetContentRegionAvail().y * 0.48f, 160.0f, 360.0f);
    catalog_grid(*data, gridHeight, gridWidth);
    if (sideBySide) {
        ImGui::SameLine(0, style.ItemSpacing.x);
        ImGui::BeginChild("##item_detail", {0, gridHeight}, ImGuiChildFlags_Borders);
    }
    selected_item(data);
    if (sideBySide) {
        ImGui::EndChild();
    }
}

void selected_item(const std::shared_ptr<const Catalog>& data) {
    if (g_selected < 0 || static_cast<std::size_t>(g_selected) >= data->entries.size()) {
        ImGui::TextDisabled("Select an item to inspect or grant it.");
        return;
    }
    const auto& entry = data->entries[g_selected];
    ImGui::SeparatorText("Selected item");
    const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 24;
    icon(entry, 72);
    if (wide) {
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", name(entry));
    ImGui::TextWrapped("ID %u | 0x%08X | bucket %u",
                       static_cast<unsigned>(entry.identity.definitionIndex),
                       entry.identity.definitionHash,
                       static_cast<unsigned>(entry.identity.bucketId));
    ImGui::TextWrapped("Category: %s", kCategoryNames[static_cast<std::size_t>(entry.category)]);
    if (!entry.itemType.empty()) {
        ImGui::TextWrapped("Installed type: %s", entry.itemType.c_str());
    }
    ImGui::SetNextItemWidth(96);
    ImGui::InputInt("Quantity", &g_quantity, 0, 0);
    namespace definitions = state::build_data;
    definitions::items::details::Definition detail{};
    const bool detailReady =
        definitions::find_configured_item_detail(entry.identity.definitionIndex, detail)
        && detail.definitionIndex == entry.identity.definitionIndex
        && detail.definitionHash == entry.identity.definitionHash
        && detail.bucketId == entry.identity.bucketId;
    g_quantity = (std::clamp)(g_quantity, 1, (std::max)(1, entry.quantityLimit));
    // Classification is the cheap mirror the whole catalog is built with. Ask the reward policy
    // itself about the one item on screen, and only when the selection or the account changes.
    if (g_probed != g_selected && g_inventory.ready && detailReady) {
        g_probe = service::grantable(entry);
        g_probed = g_selected;
    }
    const bool allowed = g_inventory.ready && detailReady && g_probe.accepted;
    ImGui::SameLine();
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Grant")) {
        grant_feedback(service::grant(entry, g_quantity));
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (!allowed) {
        if (!g_inventory.ready) {
            ImGui::TextDisabled("Select a character to grant items");
        } else {
            ImGui::TextDisabled("Unable to grant");
        }
    }
    // Where an item lands is derived from installed data, so the panel reports it rather than
    // leaving the bucket a number to look up elsewhere.
    {
        namespace buckets = definitions::inventory::buckets;
        buckets::Descriptor bucket{};
        if (definitions::find_inventory_bucket_descriptor(entry.identity.bucketId, bucket)
            && bucket.bucketId == entry.identity.bucketId) {
            constexpr std::array<const char*, 3> kArrays{"character", "profile", "small profile"};
            const auto selector = static_cast<std::size_t>(bucket.arraySelector);
            ImGui::TextDisabled("Bucket %u, %s array, slots %u-%u, stack %d%s",
                                static_cast<unsigned>(bucket.bucketId),
                                selector < kArrays.size() ? kArrays[selector] : "unknown",
                                static_cast<unsigned>(bucket.firstSlot),
                                static_cast<unsigned>(bucket.firstSlot + bucket.slotCount),
                                detail.maxStackSize,
                                (bucket.policyFlags & buckets::kFifo) != 0 ? ", FIFO" : "");
        } else {
            ImGui::TextDisabled("Bucket %u, no installed descriptor",
                                static_cast<unsigned>(entry.identity.bucketId));
        }
    }
    if (!entry.description.empty()) {
        ImGui::TextWrapped("Description: %s", entry.description.c_str());
    } else {
        ImGui::TextDisabled("Description unavailable.");
    }
    if (entry.bounty) {
        catalog_bounty(entry);
    } else {
        objectives(entry);
    }
}
constexpr std::size_t kHeldColumns = 4, kHeldRows = 7;
constexpr std::size_t kHeldPerPage = kHeldColumns * kHeldRows;
constexpr float kHeldTile = 72.0f;

/** @return Rows the current page actually fills, so the grid claims no empty space. */
std::size_t held_rows_used() noexcept {
    const auto total = g_inventory.bounties.size();
    const auto first = static_cast<std::size_t>((std::max)(g_heldPage, 1) - 1) * kHeldPerPage;
    // An empty page keeps the full grid so the container does not collapse when nothing is held.
    if (total == 0) {
        return kHeldRows;
    }
    const auto shown = first >= total ? 0 : (std::min)(total - first, kHeldPerPage);
    const std::size_t used = (shown + kHeldColumns - 1) / kHeldColumns;
    return used == 0 ? kHeldRows : used;
}

/** Held pursuits as the Character screen lays them out: four across, one page. */
void held_grid(const Catalog& data) {
    constexpr std::size_t kColumns = kHeldColumns, kPerPage = kHeldPerPage;
    constexpr float kTile = kHeldTile;
    const auto kRows = held_rows_used();
    const auto total = g_inventory.bounties.size();
    const int lastPage = (std::max)(1, static_cast<int>((total + kPerPage - 1) / kPerPage));
    g_heldPage = (std::clamp)(g_heldPage, 1, lastPage);
    ImGui::BeginDisabled(g_heldPage <= 1);
    if (ImGui::ArrowButton("##held_previous", ImGuiDir_Left)) {
        --g_heldPage;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Page %d of %d", g_heldPage, lastPage);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_heldPage >= lastPage);
    if (ImGui::ArrowButton("##held_next", ImGuiDir_Right)) {
        ++g_heldPage;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu held", total);

    auto* draw = ImGui::GetWindowDrawList();
    const auto first = static_cast<std::size_t>(g_heldPage - 1) * kPerPage;
    for (std::size_t row = 0; row < kRows; ++row) {
        for (std::size_t column = 0; column < kColumns; ++column) {
            const auto slot = first + row * kColumns + column;
            if (column != 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(row * kColumns + column));
            if (slot >= total) {
                ImGui::Dummy({kTile, kTile});
                ImGui::PopID();
                continue;
            }
            const auto& held = g_inventory.bounties[slot];
            const auto* entry = find(data, held.index, held.hash);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 corner{origin.x + kTile, origin.y + kTile};
            if (ImGui::Selectable("##slot", g_heldSelection == held.instance, 0, {kTile, kTile})) {
                select_held(held);
            }
            const auto texture = entry ? icons::get(entry->iconIndex) : ImTextureID_Invalid;
            if (texture != ImTextureID_Invalid) {
                draw->AddImage(ImTextureRef(texture), origin, corner);
            } else {
                draw->AddRect(origin, corner, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            }
            // A complete pursuit is the one worth redeeming, so it reads at a glance.
            if (held.complete) {
                draw->AddRect(origin, corner, IM_COL32(120, 220, 120, 255), 0.0f, 0, 2.0f);
            }
            if (g_heldSelection == held.instance) {
                draw->AddRect(
                    origin, corner, ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%u  %s",
                                  static_cast<unsigned>(held.index),
                                  entry ? name(*entry) : "Metadata unavailable");
            }
            ImGui::PopID();
        }
    }
}

/** The selected held pursuit and its lanes, drawn beside the grid when the page is wide. */
void held_detail(const Catalog& data) {
    for (const auto& held : g_inventory.bounties) {
        if (held.instance != g_heldSelection) {
            continue;
        }
        const auto* entry = find(data, held.index, held.hash);
        if (!entry) {
            ImGui::TextDisabled("Installed bounty details unavailable.");
            return;
        }
        icon(*entry, 64);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", name(*entry));
        ImGui::TextDisabled("Item %u", static_cast<unsigned>(held.index));
        ImGui::EndGroup();
        bounty_lanes(*entry, held);
        return;
    }
    ImGui::TextDisabled("Select a held pursuit to edit its objective lanes.");
}

/** Installed page selection and the page grant, drawn beside the grid. */
void bounty_page_controls(const service::Pages& pages, int lastPage) {
    ImGui::BeginDisabled(g_bountyPage <= 1);
    if (ImGui::ArrowButton("##bounty_previous", ImGuiDir_Left)) {
        --g_bountyPage;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##bounty_page", &g_bountyPage, 0, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_bountyPage >= lastPage);
    if (ImGui::ArrowButton("##bounty_next", ImGuiDir_Right)) {
        ++g_bountyPage;
    }
    ImGui::EndDisabled();
    g_bountyPage = (std::clamp)(g_bountyPage, 1, lastPage);
    ImGui::SameLine();
    ImGui::Text("of %d", lastPage);
    ImGui::SameLine();
    ImGui::BeginDisabled(pages.count == 0);
    if (ImGui::Button("Grant page")) {
        feedback(service::clear(service::Clear::bounties));
        const auto page = service::bounty_page(static_cast<std::size_t>(g_bountyPage));
        service::Feedback refusal{};
        const auto granted = service::grant_bounty_page(page, refusal);
        if (granted < page.size()) {
            service::Feedback shortfall{false};
            std::snprintf(shortfall.text.data(),
                          shortfall.text.size(),
                          "granted %zu of %zu; %s",
                          granted,
                          page.size(),
                          refusal.text[0] == '\0' ? "remainder refused" : refusal.text.data());
            feedback(shortfall);
        }
        g_pageExpected = granted;
        g_pageDeadline = ImGui::GetTime() + 60.0;
    }
    ImGui::EndDisabled();
    if (g_pageExpected != 0) {
        if (g_inventory.bounties.size() >= g_pageExpected || ImGui::GetTime() >= g_pageDeadline) {
            g_pageExpected = 0;
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "acquiring %zu of %zu...", g_inventory.bounties.size(), g_pageExpected);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%zu installed bounties, %zu per page. Discards held first. "
                          "%zu held entries have unresolved metadata.",
                          pages.bounties,
                          service::kBountyPageSize,
                          g_inventory.unresolved);
    }
}

void bounties_tab(const Catalog& data) {
    if (!g_inventory.ready) {
        ImGui::TextWrapped("Select a character to view held bounties.");
        return;
    }
    const auto pages = service::bounty_pages();
    const int lastPage = (std::max)(1, static_cast<int>(pages.count));
    g_bountyPage = (std::clamp)(g_bountyPage, 1, lastPage);
    const auto& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    // The grid claims exactly the tiles it draws; everything else belongs to the detail column.
    const float gridWidth = kHeldColumns * kHeldTile + (kHeldColumns - 1) * style.ItemSpacing.x
                            + style.WindowPadding.x * 2.0f;
    const float gridHeight =
        static_cast<float>(held_rows_used()) * (kHeldTile + style.ItemSpacing.y)
        + ImGui::GetFrameHeightWithSpacing() + style.WindowPadding.y * 2.0f;
    const bool sideBySide = available > gridWidth + 320.0f;
    const float height =
        sideBySide ? (std::max)(gridHeight, ImGui::GetContentRegionAvail().y) : gridHeight;
    if (ImGui::BeginChild(
            "##held_grid", {sideBySide ? gridWidth : 0.0f, gridHeight}, ImGuiChildFlags_Borders)) {
        held_grid(data);
    }
    ImGui::EndChild();
    if (sideBySide) {
        ImGui::SameLine(0, style.ItemSpacing.x);
        ImGui::BeginChild("##held_detail", {0, height}, ImGuiChildFlags_Borders);
    }
    // Page and completion controls sit beside the grid rather than above it, so the tiles keep
    // the vertical space.
    ImGui::Text("%zu held", g_inventory.bounties.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        refresh();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_inventory.bounties.empty());
    if (ImGui::Button("Complete all held")) {
        feedback(service::complete_bounties());
    }
    ImGui::EndDisabled();
    bounty_page_controls(pages, lastPage);
    ImGui::Separator();
    held_detail(data);
    if (sideBySide) {
        ImGui::EndChild();
    }
}
int g_bucketSelected = 0;
std::size_t g_bucketSelection = 0;
std::vector<service::BucketSummary> g_buckets{};
std::vector<service::BucketItem> g_bucketItems{};
std::vector<int> g_bucketQuantities{};
double g_bucketRefreshAt = 0.0;

/** Reads every bucket and the selected one's contents, at the panel's ordinary refresh rate. */
void refresh_buckets(bool force) {
    const auto now = ImGui::GetTime();
    if (!force && now < g_bucketRefreshAt) {
        return;
    }
    g_bucketRefreshAt = now + 1.0;
    g_buckets = service::buckets();
    if (g_bucketSelected < 0 || g_bucketSelected >= static_cast<int>(g_buckets.size())) {
        g_bucketItems.clear();
        g_bucketQuantities.clear();
        return;
    }
    g_bucketItems = service::bucket_items(g_buckets[g_bucketSelected].bucketId);
    g_bucketQuantities.assign(g_bucketItems.size(), 0);
    for (std::size_t i = 0; i < g_bucketItems.size(); ++i) {
        g_bucketQuantities[i] = g_bucketItems[i].quantity;
    }
}

void buckets_tab(const Catalog& data) {
    refresh_buckets(false);
    constexpr std::array<const char*, 3> kArrays{"character", "profile", "small profile"};
    constexpr float kTile = 64.0f;
    if (g_buckets.empty()) {
        ImGui::TextDisabled("No installed buckets.");
        return;
    }
    g_bucketSelected = (std::clamp)(g_bucketSelected, 0, static_cast<int>(g_buckets.size()) - 1);
    const auto label = [&](std::size_t i) {
        static char text[96];
        const auto& b = g_buckets[i];
        std::snprintf(text,
                      sizeof text,
                      "%u  %s  %zu/%u%s",
                      unsigned(b.bucketId),
                      kArrays[b.arraySelector < 3U ? b.arraySelector : 2U],
                      b.held,
                      unsigned(b.slotCount),
                      (b.policyFlags & 1U) != 0 ? "  FIFO" : "");
        return text;
    };
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo("##bucket", label(static_cast<std::size_t>(g_bucketSelected)))) {
        for (std::size_t i = 0; i < g_buckets.size(); ++i) {
            if (ImGui::Selectable(label(i), g_bucketSelected == static_cast<int>(i))) {
                g_bucketSelected = static_cast<int>(i);
                g_bucketSelection = 0;
                refresh_buckets(true);
            }
        }
        ImGui::EndCombo();
    }
    const auto& bucket = g_buckets[static_cast<std::size_t>(g_bucketSelected)];
    ImGui::SameLine();
    if (ImGui::Button("Empty bucket")) {
        feedback(service::clear_bucket(bucket.bucketId));
        refresh_buckets(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "slots %u..%u", unsigned(bucket.firstSlot), unsigned(bucket.firstSlot + bucket.slotCount));

    const auto& style = ImGui::GetStyle();
    constexpr std::size_t kColumns = 6;
    const float gridWidth =
        kColumns * kTile + (kColumns - 1) * style.ItemSpacing.x + style.WindowPadding.x * 2.0f;
    const bool sideBySide = ImGui::GetContentRegionAvail().x > gridWidth + 260.0f;
    const float height = (std::max)(200.0f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild(
        "##bucket_grid", {sideBySide ? gridWidth : 0.0f, height}, ImGuiChildFlags_Borders);
    if (g_bucketItems.empty()) {
        ImGui::TextDisabled("Nothing held here.");
    }
    auto* draw = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < g_bucketItems.size(); ++i) {
        const auto& item = g_bucketItems[i];
        const auto* entry = find(data, item.index, item.hash);
        if (i % kColumns != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 corner{origin.x + kTile, origin.y + kTile};
        const bool chosen = g_bucketSelection == i + 1;
        if (ImGui::Selectable("##cell", chosen, 0, {kTile, kTile})) {
            g_bucketSelection = i + 1;
        }
        // The icon cache holds fewer slots than the largest bucket holds rows.
        constexpr std::size_t kBucketIconBudget = 48;
        const auto texture =
            entry && i < kBucketIconBudget ? icons::get(entry->iconIndex) : ImTextureID_Invalid;
        if (texture != ImTextureID_Invalid) {
            draw->AddImage(ImTextureRef(texture), origin, corner);
        } else {
            draw->AddRect(origin, corner, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        if (item.equipped) {
            draw->AddRect(origin, corner, IM_COL32(220, 200, 120, 255), 0.0f, 0, 2.0f);
        }
        if (chosen) {
            draw->AddRect(origin, corner, ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
        }
        if (item.quantity > 1) {
            char count[16]{};
            std::snprintf(count, sizeof count, "%d", item.quantity);
            draw->AddText({origin.x + 4.0f, corner.y - ImGui::GetTextLineHeight() - 2.0f},
                          IM_COL32_WHITE,
                          count);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%u  %s", unsigned(item.index), entry ? name(*entry) : "Metadata unavailable");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (sideBySide) {
        ImGui::SameLine(0, style.ItemSpacing.x);
        ImGui::BeginChild("##bucket_detail", {0, height}, ImGuiChildFlags_Borders);
    }
    if (g_bucketSelection == 0 || g_bucketSelection > g_bucketItems.size()) {
        ImGui::TextDisabled("Select an item.");
    } else {
        const auto slot = g_bucketSelection - 1;
        const auto& item = g_bucketItems[slot];
        const auto* entry = find(data, item.index, item.hash);
        if (entry) {
            icon(*entry, 72.0f);
            ImGui::Text("%s", name(*entry));
            if (!entry->itemType.empty()) {
                ImGui::TextDisabled("%s", entry->itemType.c_str());
            }
        } else {
            ImGui::Text("Item %u", unsigned(item.index));
        }
        ImGui::Separator();
        ImGui::TextDisabled("idx=%u  serial=%d", unsigned(item.index), item.mutationSerial);
        if (item.instance != 0) {
            ImGui::TextDisabled("instance 0x%016llX",
                                static_cast<unsigned long long>(item.instance));
        }
        if (item.equipped) {
            ImGui::TextDisabled("equipped");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(item.equipped);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("##qty", &g_bucketQuantities[slot], 0, 0);
        g_bucketQuantities[slot] =
            (std::clamp)(g_bucketQuantities[slot], 0, (std::max)(1, item.maxStack));
        ImGui::SameLine();
        ImGui::TextDisabled("of %d", (std::max)(1, item.maxStack));
        if (ImGui::Button("Set quantity")) {
            feedback(
                service::set_item_quantity(item.instance, item.index, g_bucketQuantities[slot]));
            refresh_buckets(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            feedback(service::set_item_quantity(item.instance, item.index, 0));
            g_bucketSelection = 0;
            refresh_buckets(true);
        }
        ImGui::EndDisabled();
    }
    if (sideBySide) {
        ImGui::EndChild();
    }
}
void clear_tab() {
    ImGui::TextWrapped("Drops every held item in the chosen scope. Equipped items are kept.");
    ImGui::BeginDisabled(!g_inventory.ready);
    for (std::size_t i = 0; i < kClearNames.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(kClearNames[i], {140, 0})) {
            g_clearCategory = static_cast<int>(i);
            g_clearCharacter = g_inventory.character;
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", kClearDescriptions[i]);
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!g_inventory.ready) {
        ImGui::TextDisabled("Select a character to use Clear.");
    }
    if (g_clearCategory < 0) {
        return;
    }
    ImGui::Separator();
    ImGui::Text("Clear %s?", kClearNames[g_clearCategory]);
    ImGui::TextWrapped("%s", kClearDescriptions[g_clearCategory]);
    ImGui::BeginDisabled(!g_inventory.ready || g_clearCharacter != g_inventory.character);
    if (ImGui::Button("Confirm Clear")) {
        const auto category = static_cast<service::Clear>(g_clearCategory);
        g_clearCategory = -1;
        feedback(service::clear(category));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_clearCategory = -1;
    }
}
} // namespace

void draw() noexcept {
    try {
        catalog::start();
        icons::begin_frame(client::hooks::graphics::renderer::g_resources.device);
        if (ImGui::GetTime() >= g_refreshAt) {
            refresh();
        }
        const char* note = g_grantFeedback.text[0] ? g_grantFeedback.text.data()
                           : g_feedback.text[0]    ? g_feedback.text.data()
                                                   : nullptr;
        if (note) {
            // The grid owns the vertical space, so the last result stays on the header row.
            ImGui::SameLine();
            char brief[72]{};
            std::snprintf(brief, sizeof brief, "%s", note);
            ImGui::TextDisabled("%s", brief);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", note);
            }
        }
        const auto data = catalog::snapshot();
        if (!data) {
            ImGui::TextUnformatted(catalog::status());
            return;
        }
        // Allocating the search results before opening tabs keeps ImGui balanced on failure.
        prepare_filter(data);
        if (ImGui::BeginTabBar("##items_tabs")) {
            if (ImGui::BeginTabItem("Catalog")) {
                catalog_tab(data);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Pursuits")) {
                bounties_tab(*data);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Buckets")) {
                buckets_tab(*data);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Clear")) {
                clear_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } catch (...) {
        // Keep allocation failure from unwinding into Present. Catalog reload can retry.
        std::snprintf(g_feedback.text.data(),
                      g_feedback.text.size(),
                      "Items storage unavailable; reload the catalog.");
    }
}
void release_renderer() noexcept {
    catalog::stop();
    icons::release();
    g_refreshAt = 0;
}
void shutdown() noexcept {
    catalog::stop();
    g_filteredCatalog.reset();
    g_filtered.clear();
    g_inventory = {};
    g_feedback = {};
    g_grantFeedback = {};
    g_heldPage = 1;
    g_pageExpected = 0;
    g_heldSelection = 0;
    g_laneEdited.fill(false);
    g_selected = -1;
    g_refreshAt = 0;
    g_clearCategory = -1;
}
} // namespace sunrise::client::ui::items
