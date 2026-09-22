#include "items_panel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

#include "../../../server/ui/items/items_inventory_service.h"
#include "../../../state/build_data/runtime.h"
#include "../../hooks/graphics/renderer/state.h"
#include "items_catalog.h"
#include "items_icon_cache.h"
#include "items_tables.h"
#include "items_widgets.h"

namespace sunrise::client::ui::items {
namespace {
namespace service = server::ui::items;
std::array<char, 256> g_search{};
std::string g_appliedSearch;
std::shared_ptr<const Catalog> g_filteredCatalog;
std::vector<std::size_t> g_filtered;
int g_selected{-1};
int g_quantity{1};
const char* g_grantFeedback{};
const char* g_bucketFeedback{};
bool g_grantable{};
double g_probeAt{};
int g_probed{-1};
enum class Sort : int { type, name, id };
constexpr std::array<const char*, 3> kSortNames{"Type", "Name", "ID"};
int g_category{static_cast<int>(Category::all)}, g_appliedCategory{-1};
int g_sort{static_cast<int>(Sort::type)}, g_appliedSort{-1};
bool g_resetGridScroll{};
service::Inventory g_inventory;
int g_bucket{};
std::uint64_t g_heldInstance{};
std::uint32_t g_heldHash{};
std::int32_t g_heldSerial{-1};
int g_heldQuantity{1};
double g_inventoryRefreshAt{};
// Reserve cache slots for the inspector and partially visible tiles.
constexpr int kIconBudget = icons::kCapacity - 6;

int compare_text(std::string_view left, std::string_view right) noexcept {
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
    auto query = catalog::search_text(g_search.data());
    if (g_filteredCatalog != data || query != g_appliedSearch || g_category != g_appliedCategory
        || g_sort != g_appliedSort) {
        g_filtered.clear();
        for (std::size_t i = 0; i < data->entries.size(); ++i) {
            if ((g_category == static_cast<int>(Category::all)
                 || static_cast<int>(data->entries[i].category) == g_category)
                && catalog::matches(data->entries[i].search, query)) {
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
    // Leave cache slots for the selected item and partially visible rows.
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
            ImGui::TextWrapped("No matching items. Try a name, ID or hash.");
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
                        g_grantFeedback = nullptr;
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
                            widgets::image(texture, imageMin, imageMax);
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
                                      widgets::name(&entry),
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
                        ImGui::SetTooltip("%s", widgets::name(&entry));
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
    ImGui::InputTextWithHint(
        "##item_search", "Search id, hash, name or description", g_search.data(), g_search.size());
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
        ImGui::TextDisabled("%zu matches / %zu installed / %zu named",
                            g_filtered.size(),
                            data->entries.size(),
                            data->names);
    }
    if (!wideFilters) {
        ImGui::TextDisabled("%zu matches / %zu installed / %zu named",
                            g_filtered.size(),
                            data->entries.size(),
                            data->names);
    }
    const auto& style = ImGui::GetStyle();
    const float available = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
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
    widgets::icon(&entry, 72);
    if (wide) {
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", widgets::name(&entry));
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
    const auto quantityLimit =
        detailReady
                && detail.instancedDefinitionState
                       == definitions::items::details::InstancedDefinitionState::stackable
            ? (std::max)(1, detail.maxStackSize)
            : 1;
    g_quantity = (std::clamp)(g_quantity, 1, quantityLimit);
    // Probe only the selected item; a one-second refresh bounds State work on the render thread.
    if ((g_probed != g_selected || ImGui::GetTime() >= g_probeAt) && detailReady) {
        g_grantable = service::grantable(entry.identity.definitionIndex, 1);
        g_probed = g_selected;
        g_probeAt = ImGui::GetTime() + 1.0;
    }
    const bool allowed = detailReady && g_grantable;
    ImGui::SameLine();
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Grant")) {
        g_grantFeedback =
            service::grant(entry.identity.definitionIndex, static_cast<std::uint32_t>(g_quantity))
                ? "Reward queued."
                : "Reward could not be granted for the selected character.";
        g_probeAt = 0;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (g_grantFeedback) {
        ImGui::TextWrapped("%s", g_grantFeedback);
    }
    if (!allowed) {
        ImGui::TextDisabled("Unavailable for the selected character.");
    }
    {
        namespace buckets = definitions::inventory::buckets;
        buckets::Descriptor bucket{};
        if (definitions::find_inventory_bucket_descriptor(entry.identity.bucketId, bucket)
            && bucket.bucketId == entry.identity.bucketId) {
            constexpr std::array<const char*, 3> kArrays{"character", "profile", "small profile"};
            const auto selector = static_cast<std::size_t>(bucket.arraySelector);
            ImGui::TextDisabled("Bucket %u, %s array, first slot %u, capacity %u, stack %d",
                                static_cast<unsigned>(bucket.bucketId),
                                selector < kArrays.size() ? kArrays[selector] : "unknown",
                                static_cast<unsigned>(bucket.firstSlot),
                                static_cast<unsigned>(bucket.slotCount),
                                detail.maxStackSize);
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
}
void refresh_inventory() {
    if (ImGui::GetTime() < g_inventoryRefreshAt) {
        return;
    }
    g_inventoryRefreshAt = ImGui::GetTime() + 1.0;
    auto current = service::inventory();
    if (current.character != g_inventory.character) {
        g_heldHash = 0;
        g_heldSerial = -1;
        g_bucketFeedback = nullptr;
        g_grantFeedback = nullptr;
    }
    g_inventory = std::move(current);
}

void buckets_tab(const Catalog& data) {
    refresh_inventory();
    if (g_inventory.buckets.empty()) {
        ImGui::TextDisabled("Inventory is not available.");
        return;
    }
    g_bucket = (std::clamp)(g_bucket, 0, static_cast<int>(g_inventory.buckets.size()) - 1);
    const auto label = [](const service::Bucket& bucket, char (&text)[96]) {
        constexpr std::array<const char*, 3> scopes{"Character", "Account", "Small account"};
        const auto count =
            std::count_if(g_inventory.items.begin(),
                          g_inventory.items.end(),
                          [&](const auto& item) { return item.bucket == bucket.id; });
        std::snprintf(text,
                      sizeof text,
                      "%u / %s / %zu of %u",
                      unsigned(bucket.id),
                      bucket.scope < scopes.size() ? scopes[bucket.scope] : "Unknown",
                      static_cast<std::size_t>(count),
                      unsigned(bucket.capacity));
    };
    char selectedLabel[96]{};
    label(g_inventory.buckets[g_bucket], selectedLabel);
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::BeginCombo("Bucket", selectedLabel)) {
        for (std::size_t i = 0; i < g_inventory.buckets.size(); ++i) {
            char text[96]{};
            label(g_inventory.buckets[i], text);
            if (ImGui::Selectable(text, g_bucket == static_cast<int>(i))) {
                g_bucket = static_cast<int>(i);
                g_bucketFeedback = nullptr;
                g_heldHash = 0;
                g_heldSerial = -1;
            }
        }
        ImGui::EndCombo();
    }
    if (g_bucket >= static_cast<int>(g_inventory.buckets.size())) {
        return;
    }
    std::vector<const service::HeldItem*> items;
    for (const auto& item : g_inventory.items) {
        if (item.bucket == g_inventory.buckets[g_bucket].id) {
            items.push_back(&item);
        }
    }
    const auto entry_for = [&](const service::HeldItem& item) {
        const auto* entry = widgets::find(data, item.index);
        return entry && entry->identity.definitionHash == item.hash ? entry : nullptr;
    };
    if (!ImGui::BeginTable("##bucket_contents", 2, ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Selected item", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    std::vector<widgets::Card> cards;
    int selectedIndex = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = *items[i];
        cards.push_back(
            {entry_for(item), nullptr, static_cast<std::uint32_t>(item.quantity), item.equipped});
        if (item.hash == g_heldHash && item.instance == g_heldInstance) {
            selectedIndex = static_cast<int>(i);
        }
    }
    const auto picked = widgets::grid("##bucket_grid", cards, selectedIndex);
    if (picked != selectedIndex && picked >= 0) {
        g_heldHash = items[picked]->hash;
        g_bucketFeedback = nullptr;
        g_heldInstance = items[picked]->instance;
        g_heldSerial = -1;
    }
    ImGui::TableNextColumn();
    const auto selected = std::find_if(items.begin(), items.end(), [](const auto* item) {
        return item->hash == g_heldHash && item->instance == g_heldInstance;
    });
    if (selected == items.end()) {
        ImGui::TextDisabled("Select an item to inspect or edit it.");
    } else {
        const auto item = **selected;
        if (const auto* entry = entry_for(item)) {
            widgets::icon(entry, 72.0f);
            ImGui::TextWrapped("%s", widgets::name(entry));
        }
        ImGui::Text("Item %u / quantity %d", unsigned(item.index), item.quantity);
        ImGui::TextDisabled("0x%08X / serial %d", item.hash, item.serial);
        state::build_data::items::details::Definition detail{};
        if (state::build_data::find_configured_item_detail(item.index, detail)
            && detail.definitionHash == item.hash
            && detail.instancedDefinitionState
                   == state::build_data::items::details::InstancedDefinitionState::stackable
            && detail.maxStackSize > 1) {
            if (g_heldSerial != item.serial) {
                g_heldSerial = item.serial;
                g_heldQuantity = item.quantity;
            }
            ImGui::BeginDisabled(item.equipped || g_inventory.character == 0);
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("Quantity", &g_heldQuantity, 0, 0);
            g_heldQuantity = (std::clamp)(g_heldQuantity, 1, detail.maxStackSize);
            ImGui::TextDisabled("Maximum stack: %d", detail.maxStackSize);
            ImGui::BeginDisabled(g_heldQuantity == item.quantity);
            if (ImGui::Button("Set quantity")) {
                g_bucketFeedback =
                    service::set_quantity(g_inventory.character, item, g_heldQuantity)
                        ? "Stack quantity updated."
                        : "Stack changed or could not be updated.";
                g_inventoryRefreshAt = 0;
                g_heldSerial = -1;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        if (item.equipped) {
            ImGui::TextDisabled("Equipped; unequip before editing.");
        }
        ImGui::BeginDisabled(item.equipped || g_inventory.character == 0);
        if (ImGui::Button(item.quantity > 1 ? "Remove stack" : "Remove item")) {
            g_bucketFeedback = service::remove(g_inventory.character, item)
                                   ? "Item removed."
                                   : "Item changed, is equipped, or could not be removed.";
            g_heldHash = 0;
            g_inventoryRefreshAt = 0;
        }
        ImGui::EndDisabled();
    }
    if (g_bucketFeedback) {
        ImGui::TextWrapped("%s", g_bucketFeedback);
    }
    ImGui::EndTable();
}
} // namespace
void draw() noexcept {
    try {
        catalog::start();
        icons::begin_frame(client::hooks::graphics::renderer::g_resources.device);
        const auto data = catalog::snapshot();
        if (!data) {
            ImGui::TextUnformatted(catalog::status());
            return;
        }
        prepare_filter(data);
        if (ImGui::BeginTabBar("##items_tabs")) {
            if (ImGui::BeginTabItem("Catalog")) {
                catalog_tab(data);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Reward tables")) {
                tables::draw(*data);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Buckets")) {
                buckets_tab(*data);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } catch (...) {
        ImGui::TextUnformatted("Item data could not be displayed.");
    }
}
void release_renderer() noexcept {
    catalog::stop();
    icons::release();
}
void shutdown() noexcept {
    tables::clear();
    catalog::stop();
    g_filteredCatalog.reset();
    g_filtered.clear();
    g_selected = -1;
    g_grantFeedback = {};
    g_bucketFeedback = {};
    g_inventory = {};
    g_heldHash = 0;
    g_inventoryRefreshAt = 0;
}
} // namespace sunrise::client::ui::items
