#include "items_panel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <imgui.h>

#include "../../hooks/graphics/renderer/state.h"
#include "../../../state/build_data/runtime.h"
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
double g_grantPollAt{};
std::uint64_t g_heldSelection{};
std::array<int, 7> g_laneValues{};
std::array<bool, 7> g_laneEdited{};
int g_clearCategory{-1};
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
    "Reset account season pass XP and mapped reward claims for all classes. Already granted items remain."};

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
    g_grantPollAt = 0;
    g_feedback = {};
    if (!result.pending) refresh();
}
void poll_grant() {
    if (!g_grantFeedback.pending || ImGui::GetTime() < g_grantPollAt) return;
    g_grantFeedback = service::grant_receipt(g_grantFeedback.requestId);
    g_grantPollAt = ImGui::GetTime() + 0.25;
    if (!g_grantFeedback.pending) refresh();
}
const Entry* find(const Catalog& catalog, std::uint16_t index, std::uint32_t hash) noexcept {
    const auto found = std::lower_bound(catalog.entries.begin(), catalog.entries.end(), index,
        [](const Entry& entry, std::uint16_t value) { return entry.identity.definitionIndex < value; });
    return found != catalog.entries.end() && found->identity.definitionIndex == index
           && found->identity.definitionHash == hash ? &*found : nullptr;
}
const char* name(const Entry& entry) noexcept {
    return entry.name.empty() ? "Unnamed item" : entry.name.c_str();
}
void icon(const Entry& entry, float side) {
    const auto texture = icons::get(entry.iconIndex);
    if (texture != ImTextureID_Invalid) ImGui::Image(ImTextureRef(texture), {side, side});
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
        ImGui::Text("Lane %zu: objective %u / 0x%08X", i + 1,
                    static_cast<unsigned>(objective.index), objective.hash);
        if (!objective.description.empty()) ImGui::TextWrapped("%s", objective.description.c_str());
        if (objective.resolved)
            ImGui::TextDisabled("Completion: %d%s", objective.completion,
                                objective.itemProgress ? "" : " (shared / unsupported source)");
        else ImGui::TextDisabled("Objective metadata unavailable.");
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
    std::transform(query.begin(), query.end(), query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (g_filteredCatalog != data || query != g_appliedSearch
        || g_category != g_appliedCategory || g_sort != g_appliedSort) {
        g_filtered.clear();
        for (std::size_t i = 0; i < data->entries.size(); ++i)
            if ((g_category == static_cast<int>(Category::all)
                 || static_cast<int>(data->entries[i].category) == g_category)
                && matches(data->entries[i].search, query)) g_filtered.push_back(i);
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
        ImGui::TextDisabled("Objective %u / 0x%08X", static_cast<unsigned>(objective.index), objective.hash);
        ImGui::BeginDisabled(!held.editable || !objective.resolved || !objective.itemProgress);
        ImGui::SetNextItemWidth((std::min)(145.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputInt("##value", &g_laneValues[i])) g_laneEdited[i] = true;
        if (ImGui::GetContentRegionAvail().x >= 230) ImGui::SameLine();
        const bool apply = ImGui::Button("Set lane");
        ImGui::EndDisabled();
        if (!objective.resolved) ImGui::TextDisabled("Objective metadata unavailable.");
        else if (!objective.itemProgress) ImGui::TextDisabled("Shared / unsupported source; lane editing disabled.");
        ImGui::PopID();
        if (apply) {
            const auto result = service::set_lane(held.instance, held.index,
                                                   static_cast<std::uint8_t>(i + 1), g_laneValues[i]);
            if (result.accepted) g_laneEdited[i] = false;
            // Refresh replaces the vector behind held; leave the editor immediately afterward.
            feedback(result);
            return;
        }
    }
}

void catalog_bounty(const Entry& entry) {
    ImGui::SeparatorText("Held bounty");
    if (!g_inventory.ready) { ImGui::TextDisabled("Select a character to edit held objectives."); return; }
    const service::Held* selected = nullptr;
    std::size_t count = 0;
    for (const auto& held : g_inventory.bounties) {
        if (held.index != entry.identity.definitionIndex || held.hash != entry.identity.definitionHash) continue;
        ++count;
        if (!selected || held.instance == g_heldSelection) selected = &held;
    }
    if (!selected) {
        ImGui::TextWrapped("This character is not holding this bounty.");
        objectives(entry);
        return;
    }
    if (selected->instance != g_heldSelection) select_held(*selected);
    if (count > 1) {
        char preview[64]{};
        std::snprintf(preview, sizeof preview, "Instance 0x%016llX",
                      static_cast<unsigned long long>(selected->instance));
        ImGui::TextDisabled("%zu held copies; choose the instance to edit.", count);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##held_instance", preview)) {
            for (const auto& held : g_inventory.bounties) {
                if (held.index != entry.identity.definitionIndex || held.hash != entry.identity.definitionHash) continue;
                char label[80]{};
                std::snprintf(label, sizeof label, "0x%016llX%s",
                              static_cast<unsigned long long>(held.instance), held.editable ? "" : " (read-only)");
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

void catalog_grid(const Catalog& data, float requestedHeight) {
    const auto& style = ImGui::GetStyle();
    const float fontSize = ImGui::GetFontSize();
    const float padding = 8;
    const float iconSide = (std::clamp)(fontSize * 4, 56.0f, 80.0f);
    const float tileHeight = padding * 2 + iconSide + style.ItemInnerSpacing.y + fontSize * 2;
    const float rowPitch = tileHeight + style.ItemSpacing.y;
    // At most five partially visible rows of twelve tiles leave room for detail in the 64-slot cache.
    const float height = (std::min)(requestedHeight, rowPitch * 3.5f + style.WindowPadding.y * 2);
    if (ImGui::BeginChild("##item_grid", {0, height}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        if (g_resetGridScroll) { ImGui::SetScrollY(0); g_resetGridScroll = false; }
        const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
        const float minimumWidth = iconSide + padding * 2 + 24;
        const int columns = (std::clamp)(static_cast<int>((width + style.ItemSpacing.x)
                                                         / (minimumWidth + style.ItemSpacing.x)), 1, 12);
        const float tileWidth = (std::max)(1.0f, (width - style.ItemSpacing.x * (columns - 1)) / columns);
        if (g_filtered.empty()) ImGui::TextWrapped("No matching items. Try a name, ID, hash or objective.");
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>((g_filtered.size() + columns - 1) / columns), rowPitch);
        while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int column = 0; column < columns; ++column) {
                const auto offset = static_cast<std::size_t>(row) * columns + column;
                if (offset >= g_filtered.size()) break;
                if (column) ImGui::SameLine(0, style.ItemSpacing.x);
                const auto position = g_filtered[offset];
                const auto& entry = data.entries[position];
                ImGui::PushID(static_cast<int>(position));
                ImGui::PushStyleColor(ImGuiCol_Button,
                    style.Colors[g_selected == static_cast<int>(position) ? ImGuiCol_Header : ImGuiCol_FrameBg]);
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
                    const float side = (std::max)(1.0f, (std::min)(iconSide, tileWidth - padding * 2));
                    const ImVec2 imageMin{top.x + (tileWidth - side) * 0.5f, top.y + padding};
                    const ImVec2 imageMax{imageMin.x + side, imageMin.y + side};
                    const auto texture = icons::get(entry.iconIndex);
                    if (texture != ImTextureID_Invalid) draw->AddImage(ImTextureRef(texture), imageMin, imageMax);
                    else {
                        constexpr const char* missing = "Icon\nunavailable";
                        const auto size = ImGui::CalcTextSize(missing);
                        draw->AddText({top.x + (tileWidth - size.x) * 0.5f,
                                       imageMin.y + (iconSide - size.y) * 0.5f},
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled), missing);
                    }
                    const ImVec2 textMin{top.x + padding, top.y + padding + iconSide + style.ItemInnerSpacing.y};
                    const ImVec4 textClip{textMin.x, textMin.y, bottom.x - padding, bottom.y - padding};
                    draw->AddText(ImGui::GetFont(), fontSize, textMin, ImGui::GetColorU32(ImGuiCol_Text),
                                  name(entry), nullptr, (std::max)(1.0f, tileWidth - padding * 2), &textClip);
                    draw->PopClipRect();
                    if (g_selected == static_cast<int>(position))
                        draw->AddRect(top, bottom, ImGui::GetColorU32(ImGuiCol_CheckMark), style.FrameRounding);
                }
                if (hovered) ImGui::SetTooltip("%s", name(entry));
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}
void catalog_tab(const std::shared_ptr<const Catalog>& data) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##item_search", "Search id, hash, name, description or objective",
                             g_search.data(), g_search.size());
    const bool wideFilters = ImGui::GetContentRegionAvail().x >= 440;
    ImGui::SetNextItemWidth(wideFilters ? 180.0f : (std::max)(1.0f,
        ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Category").x - ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::Combo("Category", &g_category, kCategoryNames.data(), static_cast<int>(kCategoryNames.size()));
    if (wideFilters) ImGui::SameLine();
    ImGui::SetNextItemWidth(wideFilters ? 110.0f : (std::max)(1.0f,
        ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Sort").x - ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::Combo("Sort", &g_sort, kSortNames.data(), static_cast<int>(kSortNames.size()));
    if (g_category == static_cast<int>(Category::dummies))
        ImGui::TextWrapped("Only identified dummies are listed here. Unknown grant classifications remain read-only.");
    ImGui::TextDisabled("%zu matches / %zu installed items / %zu grant-classified",
                        g_filtered.size(), data->entries.size(), data->classified);
    const float height = (std::clamp)(ImGui::GetContentRegionAvail().y * 0.48f, 160.0f, 360.0f);
    catalog_grid(*data, height);
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
    ImGui::TextWrapped("ID %u | 0x%08X | bucket %u", static_cast<unsigned>(entry.identity.definitionIndex),
                entry.identity.definitionHash, static_cast<unsigned>(entry.identity.bucketId));
    ImGui::TextWrapped("Category: %s", kCategoryNames[static_cast<std::size_t>(entry.category)]);
    if (!entry.itemType.empty()) ImGui::TextWrapped("Installed type: %s", entry.itemType.c_str());
    if (!entry.grantVariant.empty()) ImGui::TextWrapped("%s", entry.grantVariant.c_str());
    ImGui::SetNextItemWidth(130);
    ImGui::InputInt("Quantity", &g_quantity);
    namespace definitions = state::build_data;
    definitions::items::details::Definition detail{};
    const bool detailReady = definitions::find_configured_item_detail(entry.identity.definitionIndex, detail)
                             && detail.definitionIndex == entry.identity.definitionIndex
                             && detail.definitionHash == entry.identity.definitionHash
                             && detail.bucketId == entry.identity.bucketId;
    const bool instanced = detailReady && detail.instancedDefinitionState
                                             == definitions::items::details::InstancedDefinitionState::instanced;
    // The queued grant service accepts at most nine instanced copies, not nine profile units.
    const int quantityLimit = instanced ? (std::min)(9, entry.quantityLimit) : entry.quantityLimit;
    g_quantity = (std::clamp)(g_quantity, 1, (std::max)(1, quantityLimit));
    const bool allowed = g_inventory.ready && detailReady && entry.policy == GrantPolicy::legitimate
                         && !g_grantFeedback.pending;
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Grant item")) grant_feedback(service::grant(entry, g_quantity));
    ImGui::EndDisabled();
    if (instanced && entry.quantityLimit > 9) ImGui::TextDisabled("Up to 9 copies per grant.");
    if (g_grantFeedback.pending) ImGui::TextDisabled("Grant pending...");
    ImGui::EndGroup();
    if (entry.policy == GrantPolicy::dummy)
        ImGui::TextWrapped("Role: dummy / preview or reward marker. Minting is disabled, regardless of its displayed name or type.");
    else if (entry.policy == GrantPolicy::unknown)
        ImGui::TextWrapped("Grant disabled: no positive installed grant classification. Unknown entries are read-only.");
    else if (!detailReady) ImGui::TextDisabled("Grant disabled: installed item details unavailable.");
    else if (!g_inventory.ready) ImGui::TextDisabled("Select a character to grant items.");
    if (!entry.description.empty()) ImGui::TextWrapped("%s", entry.description.c_str());
    else ImGui::TextDisabled("Description unavailable.");
    if (entry.bounty) catalog_bounty(entry);
    else objectives(entry);
}
void bounties_tab(const Catalog& data) {
    if (!g_inventory.ready) { ImGui::TextWrapped("Select a character to view held bounties."); return; }
    ImGui::Text("%zu held bounties", g_inventory.bounties.size());
    ImGui::BeginDisabled(g_inventory.bounties.empty());
    if (ImGui::Button("Complete all held bounties")) feedback(service::complete_bounties());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh();
    ImGui::TextWrapped("Completes eligible held bounty objectives without granting or redeeming rewards.");
    if (g_inventory.unresolved) ImGui::TextDisabled("%zu inventory entries have unresolved metadata.", g_inventory.unresolved);
    if (ImGui::BeginListBox("##held_bounties", { -1, 155 })) {
        for (const auto& held : g_inventory.bounties) {
            const auto* entry = find(data, held.index, held.hash);
            char label[384]{};
            std::snprintf(label, sizeof label, "%u  %s##%llu", static_cast<unsigned>(held.index),
                          entry ? name(*entry) : "Metadata unavailable",
                          static_cast<unsigned long long>(held.instance));
            if (ImGui::Selectable(label, g_heldSelection == held.instance)) {
                select_held(held);
            }
        }
        ImGui::EndListBox();
    }
    for (const auto& held : g_inventory.bounties) {
        if (held.instance != g_heldSelection) continue;
        const auto* entry = find(data, held.index, held.hash);
        if (!entry) { ImGui::TextDisabled("Installed bounty details unavailable."); return; }
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
    ImGui::TextWrapped("Clear uses the same scope and behavior as the console dropall commands.");
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
        poll_grant();
        if (ImGui::GetTime() >= g_refreshAt) refresh();
        ImGui::TextUnformatted(catalog::status());
        ImGui::SameLine();
        if (ImGui::Button("Reload catalog")) {
            icons::release();
            catalog::reload();
            g_filteredCatalog.reset();
            g_selected = -1;
        }
        if (g_grantFeedback.text[0]) ImGui::TextWrapped("Grant: %s", g_grantFeedback.text.data());
        if (g_feedback.text[0]) ImGui::TextWrapped("%s", g_feedback.text.data());
        const auto data = catalog::snapshot();
        if (!data) return;
        // Allocating the search results before opening tabs keeps ImGui balanced on failure.
        prepare_filter(data);
        if (ImGui::BeginTabBar("##items_tabs")) {
            if (ImGui::BeginTabItem("Catalog")) { catalog_tab(data); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Held bounties")) { bounties_tab(*data); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Clear")) { clear_tab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    } catch (...) {
        // Keep allocation failure from unwinding into Present. Catalog reload can retry.
        std::snprintf(g_feedback.text.data(), g_feedback.text.size(), "Items storage unavailable; reload the catalog.");
    }
}
void release_renderer() noexcept {
    catalog::stop();
    icons::release();
    g_refreshAt = g_grantPollAt = 0;
}
void shutdown() noexcept {
    catalog::stop();
    g_filteredCatalog.reset();
    g_filtered.clear();
    g_inventory = {};
    g_feedback = {};
    g_grantFeedback = {};
    g_grantPollAt = 0;
    g_heldSelection = 0;
    g_laneEdited.fill(false);
    g_selected = -1;
    g_refreshAt = 0;
    g_clearCategory = -1;
}
} // namespace sunrise::client::ui::items
