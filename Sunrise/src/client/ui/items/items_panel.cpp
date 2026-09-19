#include "items_panel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <string_view>

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
    "Reset account season pass XP and mapped reward claims for all classes. Already granted items "
    "remain."};

void select_held(const service::Held& held) {
    g_heldSelection = held.instance;
    std::copy_n(held.values.begin() + 1, 7, g_laneValues.begin());
    g_laneEdited.fill(false);
}
void refresh() {
    const auto previous = g_inventory.character;
    g_inventory = service::inventory();
    g_refreshAt = ImGui::GetTime() + 1.0;
    if (previous != g_inventory.character) {
        g_heldSelection = 0;
        g_clearCategory = -1;
    }
    for (const auto& held : g_inventory.bounties) {
        if (held.instance != g_heldSelection) continue;
        for (std::size_t lane = 0; lane < g_laneValues.size(); ++lane)
            if (!g_laneEdited[lane]) g_laneValues[lane] = held.values[lane + 1];
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
    if (texture != ImTextureID_Invalid)
        ImGui::Image(ImTextureRef(texture), {side, side});
    else {
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
        if (!objective.description.empty()) ImGui::TextWrapped("%s", objective.description.c_str());
        if (objective.resolved)
            ImGui::TextDisabled("Completion: %d%s",
                                objective.completion,
                                objective.itemProgress ? "" : " (shared / unsupported source)");
        else
            ImGui::TextDisabled("Objective metadata unavailable.");
    }
}
bool matches(std::string_view haystack, std::string_view query) noexcept {
    while (!query.empty()) {
        const auto first = query.find_first_not_of(" \t");
        if (first == std::string_view::npos) return true;
        query.remove_prefix(first);
        const auto end = query.find_first_of(" \t");
        const auto word = query.substr(0, end);
        if (haystack.find(word) == std::string_view::npos) return false;
        if (end == std::string_view::npos) return true;
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
        if (a != b) return a < b ? -1 : 1;
    }
    return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
}
int compare_optional_text(std::string_view left, std::string_view right) noexcept {
    if (left.empty() != right.empty()) return left.empty() ? 1 : -1;
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
        for (std::size_t i = 0; i < data->entries.size(); ++i)
            if ((g_category == static_cast<int>(Category::all)
                 || static_cast<int>(data->entries[i].category) == g_category)
                && matches(data->entries[i].search, query))
                g_filtered.push_back(i);
        // Sort the view indices only: selected items, held instances and ID lookup stay stable.
        std::sort(g_filtered.begin(), g_filtered.end(), [&](std::size_t left, std::size_t right) {
            const auto& a = data->entries[left];
            const auto& b = data->entries[right];
            if (g_sort == static_cast<int>(Sort::type)) {
                if (a.category != b.category) return a.category < b.category;
                const int type = compare_optional_text(a.itemType, b.itemType);
                if (type != 0) return type < 0;
            }
            if (g_sort != static_cast<int>(Sort::id)) {
                const int name = compare_optional_text(a.name, b.name);
                if (name != 0) return name < 0;
            }
            if (a.identity.definitionIndex != b.identity.definitionIndex)
                return a.identity.definitionIndex < b.identity.definitionIndex;
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
    if (!held.editable) ImGui::TextWrapped("%s", held.reason);
    for (std::size_t i = 0; i < entry.objectives.size() && i < g_laneValues.size(); ++i) {
        const auto& objective = entry.objectives[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Text("Lane %zu: %d / %d", i + 1, held.values[i + 1], objective.completion);
        if (!objective.description.empty()) ImGui::TextWrapped("%s", objective.description.c_str());
        ImGui::TextDisabled(
            "Objective %u / 0x%08X", static_cast<unsigned>(objective.index), objective.hash);
        ImGui::BeginDisabled(!held.editable || !objective.resolved || !objective.itemProgress);
        ImGui::SetNextItemWidth((std::min)(145.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputInt("##value", &g_laneValues[i], 0, 0)) g_laneEdited[i] = true;
        if (ImGui::GetContentRegionAvail().x >= 230) ImGui::SameLine();
        const bool apply = ImGui::Button("Set lane");
        ImGui::EndDisabled();
        if (!objective.resolved)
            ImGui::TextDisabled("Objective metadata unavailable.");
        else if (!objective.itemProgress)
            ImGui::TextDisabled("Shared / unsupported source; lane editing disabled.");
        ImGui::PopID();
        if (apply) {
            const auto result = service::set_lane(
                held.instance, held.index, static_cast<std::uint8_t>(i + 1), g_laneValues[i]);
            if (result.accepted) g_laneEdited[i] = false;
            // Refresh replaces the vector behind held; leave the editor immediately afterward.
            feedback(result);
            return;
        }
    }
}

/** Acquires the bounty if it is not held, completes it, and leaves redemption to the vendor. */
void page_bounty_button(const Entry& entry) {
    ImGui::BeginDisabled(!g_inventory.ready);
    if (ImGui::Button("Grant and complete")) feedback(service::page_bounty(entry));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(page)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Grants or reuses this bounty and completes its objectives. Rewards "
                          "stay unclaimed so redemption can be tested at the vendor.");
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
            || held.hash != entry.identity.definitionHash)
            continue;
        ++count;
        if (!selected || held.instance == g_heldSelection) selected = &held;
    }
    if (!selected) {
        ImGui::TextWrapped("This character is not holding this bounty.");
        page_bounty_button(entry);
        objectives(entry);
        return;
    }
    if (selected->instance != g_heldSelection) select_held(*selected);
    page_bounty_button(entry);
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
                    || held.hash != entry.identity.definitionHash)
                    continue;
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
                if (held.instance == g_heldSelection) ImGui::SetItemDefaultFocus();
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
            (std::max)(1.0f, (width - style.ItemSpacing.x * (columns - 1)) / columns);
        if (g_filtered.empty())
            ImGui::TextWrapped("No matching items. Try a name, ID, hash or objective.");
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>((g_filtered.size() + columns - 1) / columns), rowPitch);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const auto offset = static_cast<std::size_t>(row) * columns + column;
                    if (offset >= g_filtered.size()) break;
                    if (column) ImGui::SameLine(0, style.ItemSpacing.x);
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
                        if (texture != ImTextureID_Invalid)
                            draw->AddImage(ImTextureRef(texture), imageMin, imageMax);
                        else {
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
                        if (g_selected == static_cast<int>(position))
                            draw->AddRect(top,
                                          bottom,
                                          ImGui::GetColorU32(ImGuiCol_CheckMark),
                                          style.FrameRounding);
                    }
                    if (hovered) ImGui::SetTooltip("%s", name(entry));
                    ImGui::PopID();
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
    if (wideFilters) ImGui::SameLine();
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
    if (g_category == static_cast<int>(Category::dummies))
        ImGui::TextWrapped("Only identified dummies are listed here. Items this build cannot "
                           "classify remain read-only.");
    if (!wideFilters)
        ImGui::TextDisabled("%zu matches / %zu installed / %zu grantable",
                            g_filtered.size(),
                            data->entries.size(),
                            data->classified);
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
    if (sideBySide) ImGui::EndChild();
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
    if (wide) ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", name(entry));
    ImGui::TextWrapped("ID %u | 0x%08X | bucket %u",
                       static_cast<unsigned>(entry.identity.definitionIndex),
                       entry.identity.definitionHash,
                       static_cast<unsigned>(entry.identity.bucketId));
    ImGui::TextWrapped("Category: %s", kCategoryNames[static_cast<std::size_t>(entry.category)]);
    if (!entry.itemType.empty()) ImGui::TextWrapped("Installed type: %s", entry.itemType.c_str());
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
    const bool allowed =
        g_inventory.ready && detailReady && entry.policy == GrantPolicy::legitimate;
    ImGui::SameLine();
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Grant")) grant_feedback(service::grant(entry, g_quantity));
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (!allowed) {
        const char* refusal = entry.policy != GrantPolicy::legitimate ? "Cannot be granted"
                              : !detailReady ? "Cannot be granted: item details unavailable"
                                             : "Select a character to grant items";
        ImGui::TextDisabled("%s", refusal);
        if (ImGui::IsItemHovered() && entry.policy == GrantPolicy::dummy)
            ImGui::SetTooltip("Preview or reward marker, not a resident item.");
    }
    if (!entry.description.empty())
        ImGui::TextWrapped("Description: %s", entry.description.c_str());
    else
        ImGui::TextDisabled("Description unavailable.");
    if (entry.bounty)
        catalog_bounty(entry);
    else
        objectives(entry);
}
/** @return True when every declared objective of one held pursuit has reached its value. */
bool held_complete(const Entry& entry, const service::Held& held) noexcept {
    if (entry.objectives.empty()) return false;
    for (std::size_t lane = 0; lane < entry.objectives.size() && lane + 1 < held.values.size();
         ++lane) {
        const auto& objective = entry.objectives[lane];
        if (!objective.resolved || !objective.itemProgress
            || held.values[lane + 1] < objective.completion)
            return false;
    }
    return true;
}

/** Held pursuits as the Character screen lays them out: four across, seven down, one page. */
void held_grid(const Catalog& data) {
    constexpr std::size_t kColumns = 4, kRows = 7, kPerPage = kColumns * kRows;
    constexpr float kTile = 56.0f;
    const auto total = g_inventory.bounties.size();
    const int lastPage = (std::max)(1, static_cast<int>((total + kPerPage - 1) / kPerPage));
    g_heldPage = (std::clamp)(g_heldPage, 1, lastPage);
    ImGui::BeginDisabled(g_heldPage <= 1);
    if (ImGui::ArrowButton("##held_previous", ImGuiDir_Left)) --g_heldPage;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Page %d of %d", g_heldPage, lastPage);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_heldPage >= lastPage);
    if (ImGui::ArrowButton("##held_next", ImGuiDir_Right)) ++g_heldPage;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu held", total);

    auto* draw = ImGui::GetWindowDrawList();
    const auto first = static_cast<std::size_t>(g_heldPage - 1) * kPerPage;
    for (std::size_t row = 0; row < kRows; ++row) {
        for (std::size_t column = 0; column < kColumns; ++column) {
            const auto slot = first + row * kColumns + column;
            if (column != 0) ImGui::SameLine();
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
            if (ImGui::Selectable("##slot", g_heldSelection == held.instance, 0, {kTile, kTile}))
                select_held(held);
            const auto texture = entry ? icons::get(entry->iconIndex) : ImTextureID_Invalid;
            if (texture != ImTextureID_Invalid)
                draw->AddImage(ImTextureRef(texture), origin, corner);
            else
                draw->AddRect(origin, corner, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            // A complete pursuit is the one worth redeeming, so it reads at a glance.
            if (entry && held_complete(*entry, held))
                draw->AddRect(origin, corner, IM_COL32(120, 220, 120, 255), 0.0f, 0, 2.0f);
            if (g_heldSelection == held.instance)
                draw->AddRect(
                    origin, corner, ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%u  %s",
                                  static_cast<unsigned>(held.index),
                                  entry ? name(*entry) : "Metadata unavailable");
            ImGui::PopID();
        }
    }
}

void bounties_tab(const Catalog& data) {
    if (!g_inventory.ready) {
        ImGui::TextWrapped("Select a character to view held bounties.");
        return;
    }
    ImGui::Text("%zu held pursuits", g_inventory.bounties.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_inventory.bounties.empty());
    if (ImGui::Button("Complete all held")) feedback(service::complete_bounties());
    ImGui::EndDisabled();
    const auto pages = service::bounty_pages();
    const int lastPage = (std::max)(1, static_cast<int>(pages.count));
    g_bountyPage = (std::clamp)(g_bountyPage, 1, lastPage);
    ImGui::BeginDisabled(g_bountyPage <= 1);
    if (ImGui::ArrowButton("##bounty_previous", ImGuiDir_Left)) --g_bountyPage;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##bounty_page", &g_bountyPage, 0, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_bountyPage >= lastPage);
    if (ImGui::ArrowButton("##bounty_next", ImGuiDir_Right)) ++g_bountyPage;
    ImGui::EndDisabled();
    g_bountyPage = (std::clamp)(g_bountyPage, 1, lastPage);
    ImGui::SameLine();
    ImGui::Text("of %d", lastPage);
    ImGui::SameLine();
    ImGui::BeginDisabled(pages.count == 0);
    if (ImGui::Button("Grant page"))
        feedback(service::grant_bounty_page(static_cast<std::size_t>(g_bountyPage)));
    ImGui::EndDisabled();
    ImGui::TextDisabled("%zu installed bounties, %zu per page. Discards held first.",
                        pages.bounties,
                        service::kBountyPageSize);
    if (g_inventory.unresolved)
        ImGui::TextDisabled("%zu inventory entries have unresolved metadata.",
                            g_inventory.unresolved);
    held_grid(data);
    for (const auto& held : g_inventory.bounties) {
        if (held.instance != g_heldSelection) continue;
        const auto* entry = find(data, held.index, held.hash);
        if (!entry) {
            ImGui::TextDisabled("Installed bounty details unavailable.");
            return;
        }
        icon(*entry, 64);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", name(*entry));
        ImGui::EndGroup();
        bounty_lanes(*entry, held);
        return;
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
    if (!g_inventory.ready) ImGui::TextDisabled("Select a character to use Clear.");
    if (g_clearCategory < 0) return;
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
    if (ImGui::Button("Cancel")) g_clearCategory = -1;
}
} // namespace

void draw() noexcept {
    try {
        catalog::start();
        icons::begin_frame(client::hooks::graphics::renderer::g_resources.device);
        if (ImGui::GetTime() >= g_refreshAt) refresh();
        const char* note = g_grantFeedback.text[0] ? g_grantFeedback.text.data()
                           : g_feedback.text[0]    ? g_feedback.text.data()
                                                   : nullptr;
        if (note) {
            // The grid owns the vertical space, so the last result stays on the header row.
            ImGui::SameLine();
            char brief[72]{};
            std::snprintf(brief, sizeof brief, "%s", note);
            ImGui::TextDisabled("%s", brief);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", note);
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
    g_heldSelection = 0;
    g_laneEdited.fill(false);
    g_selected = -1;
    g_refreshAt = 0;
    g_clearCategory = -1;
}
} // namespace sunrise::client::ui::items
