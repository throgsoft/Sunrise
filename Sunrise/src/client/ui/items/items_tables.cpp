#include "items_tables.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../../state/build_data/rewards/reward_catalog.h"
#include "../../../state/build_data/season_pass/season_pass_catalog.h"
#include "items_icon_cache.h"
#include "items_widgets.h"

namespace sunrise::client::ui::items::tables {
namespace {
namespace rewards = state::build_data::rewards;
struct PoolDisplay {
    std::string label, search;
    std::vector<std::uint16_t> sources;
};
struct Snapshot {
    std::vector<rewards::Pool> pools;
    std::vector<rewards::Entry> entries;
    std::vector<rewards::Item> items;
    std::vector<rewards::Instruction> instructions;
    std::vector<rewards::Modifier> modifiers;
    std::vector<rewards::SocketOverride> sockets;
    std::vector<state::build_data::season_pass::Reward> pass;
    std::vector<PoolDisplay> display;
    std::vector<std::array<std::vector<int>, 2>> passRanks;
};
std::unique_ptr<Snapshot> g_data;
std::array<char, 128> g_search{};
int g_pool{};
int g_entry{-1};
std::vector<int> g_history;
bool g_openPools{};
int g_passPage{};
int g_passRank{};
int g_passTrack{};
int g_passReward{-1};
constexpr int kRanksPerPage = 10;
constexpr std::array<const char*, 2> kPassTracks{"Regular", "Premium"};

bool premium(const state::build_data::season_pass::Reward& row) noexcept {
    using Read = rewards::BankRead;
    // Premium rows start with the account-or-character pass ownership condition.
    return row.conditionCount >= 3
           && row.condition[0].opcode == static_cast<std::uint32_t>(Read::accountFlag)
           && row.condition[1].opcode == static_cast<std::uint32_t>(Read::characterFlag)
           && row.condition[2].opcode == 3;
}

void append_identity(std::string& text, std::uint32_t index, std::uint32_t hash) {
    char id[64]{};
    std::snprintf(id, sizeof id, " %u %u 0x%08X ", index, hash, hash);
    text += id;
}

bool load(const Catalog& catalog) noexcept {
    try {
        auto next = std::make_unique<Snapshot>();
        if (!rewards::read(next.get(), [](void* context, rewards::View view) noexcept {
                try {
                    auto& out = *static_cast<Snapshot*>(context);
                    out.pools.assign(view.pools.begin(), view.pools.end());
                    out.entries.assign(view.entries.begin(), view.entries.end());
                    out.items.assign(view.items.begin(), view.items.end());
                    out.instructions.assign(view.instructions.begin(), view.instructions.end());
                    out.modifiers.assign(view.modifiers.begin(), view.modifiers.end());
                    out.sockets.assign(view.sockets.begin(), view.sockets.end());
                    return true;
                } catch (...) {
                    return false;
                }
            }))
            return false;
        next->pass.resize(state::build_data::season_pass::kRewardCapacity);
        std::size_t count = 0;
        if (!state::build_data::season_pass::snapshot(next->pass, count)) return false;
        next->pass.resize(count);
        std::size_t lastRank = 0;
        for (const auto& row : next->pass)
            lastRank = (std::max)(lastRank, std::size_t(row.requiredRank));
        next->passRanks.resize(lastRank + 1);
        for (std::size_t i = 0; i < next->pass.size(); ++i) {
            const auto& row = next->pass[i];
            next->passRanks[row.requiredRank][premium(row) ? 1 : 0].push_back(static_cast<int>(i));
        }
        next->display.resize(next->pools.size());
        for (std::size_t i = 0; i < next->items.size(); ++i) {
            const auto pool = next->items[i].poolIndex;
            if (pool < next->display.size())
                next->display[pool].sources.push_back(static_cast<std::uint16_t>(i));
        }
        for (std::size_t i = 0; i < next->pools.size(); ++i) {
            auto& display = next->display[i];
            for (auto source : display.sources) {
                const auto* item = widgets::find(catalog, source);
                if (item && !item->name.empty()) {
                    display.label = item->name;
                    if (display.sources.size() > 1)
                        display.label += " (+" + std::to_string(display.sources.size() - 1) + ")";
                    break;
                }
            }
            if (display.label.empty()) {
                const auto& pool = next->pools[i];
                std::size_t named = 0;
                for (const auto& row :
                     std::span(next->entries).subspan(pool.entries.first, pool.entries.count)) {
                    const auto* item = widgets::find(catalog, row.itemIndex);
                    if (!item || item->name.empty()) continue;
                    if (named++ == 2) {
                        display.label += ", ...";
                        break;
                    }
                    display.label += display.label.empty() ? "Contains: " : ", ";
                    display.label += item->name;
                }
                if (display.label.empty()) display.label = "Pool " + std::to_string(i);
            }
            std::vector<std::size_t> pending{i};
            std::vector<bool> visited(next->pools.size());
            while (!pending.empty()) {
                const auto index = pending.back();
                pending.pop_back();
                if (visited[index]) continue;
                visited[index] = true;
                const auto& pool = next->pools[index];
                append_identity(
                    display.search, static_cast<std::uint32_t>(index), pool.definitionHash);
                for (auto source : next->display[index].sources)
                    if (const auto* item = widgets::find(catalog, source))
                        display.search += " " + item->search;
                for (const auto& entry :
                     std::span(next->entries).subspan(pool.entries.first, pool.entries.count)) {
                    if (const auto* item = widgets::find(catalog, entry.itemIndex))
                        display.search += " " + item->search;
                    if (entry.poolIndex < next->pools.size()) pending.push_back(entry.poolIndex);
                }
            }
        }
        g_data = std::move(next);
        g_pool = 0;
        return true;
    } catch (...) {
        return false;
    }
}

const char* item_name(const Catalog& catalog, std::uint16_t index) noexcept {
    return widgets::name(widgets::find(catalog, index));
}

bool contains(std::string_view text, std::string_view word) noexcept {
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::search(text.begin(),
                       text.end(),
                       word.begin(),
                       word.end(),
                       [&](unsigned char a, unsigned char b) { return fold(a) == fold(b); })
           != text.end();
}
bool matches_text(std::string_view text) noexcept {
    std::string_view query(g_search.data());
    while (!query.empty()) {
        const auto first = query.find_first_not_of(" \t");
        if (first == std::string_view::npos) break;
        query.remove_prefix(first);
        const auto end = query.find_first_of(" \t");
        if (!contains(text, query.substr(0, end))) return false;
        if (end == std::string_view::npos) break;
        query.remove_prefix(end);
    }
    return true;
}
void select_pool(int index) {
    if (index < 0 || index >= static_cast<int>(g_data->pools.size())) return;
    if (index == g_pool) {
        g_entry = -1;
        return;
    }
    g_history.push_back(g_pool);
    g_pool = index;
    g_entry = -1;
}

const char* operation(std::uint32_t opcode) noexcept {
    using R = rewards::BankRead;
    switch (opcode) {
    case static_cast<std::uint32_t>(R::accountFlag):
        return "Account flag";
    case static_cast<std::uint32_t>(R::characterFlag):
        return "Character flag";
    case static_cast<std::uint32_t>(R::accountValue):
        return "Account value";
    case static_cast<std::uint32_t>(R::characterValue):
        return "Character value";
    case static_cast<std::uint32_t>(R::externalFlag):
        return "Computed flag hash";
    case static_cast<std::uint32_t>(R::externalValue):
        return "Computed value hash";
    case 2:
        return "NOT";
    case 3:
        return "OR";
    case 4:
        return "AND";
    case 8:
        return "Equal";
    case 11:
        return "Constant";
    case 13:
        return "Greater";
    case 14:
        return "Greater or equal";
    case 15:
        return "Less";
    case 22:
        return "Negate";
    default:
        return "Native operator";
    }
}
void condition(std::span<const rewards::Instruction> code) noexcept {
    if (code.empty()) ImGui::TextDisabled("Unconditional");
    for (const auto& row : code)
        ImGui::TextWrapped("%s (%u), operand %u / 0x%08X",
                           operation(row.opcode),
                           row.opcode,
                           row.operand,
                           row.operand);
}
void reference(const char* label, std::uint16_t index) {
    if (index == rewards::kAbsent)
        ImGui::Text("%s: None", label);
    else
        ImGui::Text("%s: %u", label, index);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Raw index: %u / 0x%04X", index, index);
}
void sockets(const Catalog& catalog, std::span<const rewards::SocketOverride> rows) noexcept {
    for (const auto& row : rows) {
        ImGui::TextWrapped("Socket type %u: plug %u (%s)",
                           row.socketType,
                           row.plugItem,
                           item_name(catalog, row.plugItem));
        ImGui::TextDisabled(
            "Plug set %u, roll set %u, selection %u", row.plugSet, row.rollSet, row.selection);
    }
}
void entry_details(const Catalog& catalog, const rewards::Entry& entry) {
    const auto& data = *g_data;
    if (const auto* item = widgets::find(catalog, entry.itemIndex)) {
        widgets::icon(item, 56);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", widgets::name(item));
        ImGui::TextDisabled("%s", item->itemType.c_str());
        ImGui::EndGroup();
        ImGui::TextWrapped("Item %u / 0x%08X", entry.itemIndex, item->identity.definitionHash);
        if (!item->description.empty()) ImGui::TextWrapped("%s", item->description.c_str());
    }
    if (entry.poolIndex < data.pools.size()) {
        ImGui::TextWrapped("%s", data.display[entry.poolIndex].label.c_str());
        ImGui::TextDisabled(
            "Pool %u / 0x%08X", entry.poolIndex, data.pools[entry.poolIndex].definitionHash);
    }
    ImGui::Text("Quantity %u / weight %g", entry.quantity, entry.weight);
    if (entry.poolIndex < data.pools.size() && ImGui::Button("Open nested pool"))
        select_pool(entry.poolIndex);
    if (ImGui::CollapsingHeader("Conditions")) {
        condition(
            std::span(data.instructions).subspan(entry.condition.first, entry.condition.count));
        for (const auto& modifier :
             std::span(data.modifiers).subspan(entry.modifiers.first, entry.modifiers.count)) {
            ImGui::Text("Weight modifier %g", modifier.value);
            reference("Value index", modifier.valueIndex);
            condition(std::span(data.instructions)
                          .subspan(modifier.condition.first, modifier.condition.count));
        }
    }
    if (entry.sockets.count && ImGui::CollapsingHeader("Socket overrides"))
        sockets(catalog, std::span(data.sockets).subspan(entry.sockets.first, entry.sockets.count));
    if (ImGui::CollapsingHeader("Definition fields")) {
        reference("Item", entry.itemIndex);
        reference("Item type", entry.itemType);
        reference("Nested pool", entry.poolIndex);
        reference("Reward mapping", entry.mappingIndex);
        reference("Adjuster", entry.adjusterIndex);
        ImGui::Text("Category: 0x%08X", entry.categoryHash);
        ImGui::Text("Bucket: 0x%08X", entry.bucketHash);
        ImGui::Text("Scale: %g", entry.scale);
    }
}

bool item_row(const char* id, const Entry* item, const char* label, bool selected) {
    const auto origin = ImGui::GetCursorScreenPos();
    const auto side = ImGui::GetFrameHeight();
    const auto width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable(id, selected, 0, {0, side});
    if (ImGui::IsItemVisible()) {
        auto* draw = ImGui::GetWindowDrawList();
        const auto texture = item ? icons::get(item->iconIndex) : ImTextureID_Invalid;
        if (texture != ImTextureID_Invalid)
            draw->AddImage(ImTextureRef(texture), origin, {origin.x + side, origin.y + side});
        const ImVec2 text{origin.x + side + ImGui::GetStyle().ItemSpacing.x,
                          origin.y + (side - ImGui::GetTextLineHeight()) * 0.5f};
        draw->PushClipRect(text, {origin.x + width, origin.y + side}, true);
        draw->AddText(text, ImGui::GetColorU32(ImGuiCol_Text), label);
        draw->PopClipRect();
    }
    return clicked;
}

const Entry* pool_item(const Catalog& catalog, int index) noexcept {
    for (auto source : g_data->display[index].sources) {
        const auto* item = widgets::find(catalog, source);
        if (item && item->iconIndex != package::kNoIcon) return item;
    }
    return nullptr;
}

void reward_icon(const Entry* item, ImVec2 origin, float side) {
    auto* draw = ImGui::GetWindowDrawList();
    const auto texture = item ? icons::get(item->iconIndex) : ImTextureID_Invalid;
    if (texture != ImTextureID_Invalid) {
        draw->AddImage(ImTextureRef(texture), origin, {origin.x + side, origin.y + side});
    } else if (!item) {
        const auto color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        draw->AddRectFilled({origin.x + side * 0.12f, origin.y + side * 0.2f},
                            {origin.x + side * 0.48f, origin.y + side * 0.4f},
                            color,
                            2);
        draw->AddRectFilled({origin.x + side * 0.12f, origin.y + side * 0.32f},
                            {origin.x + side * 0.88f, origin.y + side * 0.8f},
                            color,
                            2);
    }
}

bool tree_row(const char* id,
              ImGuiTreeNodeFlags flags,
              const Entry* item,
              const char* label,
              std::uint32_t quantity = 0) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float side = ImGui::GetFrameHeight();
    const float labelOffset = ImGui::GetTreeNodeToLabelSpacing();
    const bool open = ImGui::TreeNodeEx(id, flags | ImGuiTreeNodeFlags_FramePadding, "%s", " ");
    if (ImGui::IsItemVisible()) {
        reward_icon(item, {origin.x + labelOffset, origin.y}, side);
        char text[512]{};
        if (quantity)
            std::snprintf(text, sizeof text, "%s x%u", label, quantity);
        else
            std::snprintf(text, sizeof text, "%s", label);
        const ImVec2 pos{origin.x + labelOffset + side + ImGui::GetStyle().ItemSpacing.x,
                         origin.y + (side - ImGui::GetTextLineHeight()) * 0.5f};
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(pos, ImGui::GetItemRectMax(), true);
        draw->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), text);
        draw->PopClipRect();
    }
    return open;
}

void source_items(const Catalog& catalog, int poolIndex) {
    const auto& data = *g_data;
    if (data.display[poolIndex].sources.empty()) {
        ImGui::TextDisabled("No item links directly to this pool.");
        return;
    }
    ImGui::TextWrapped("These items use this pool to select their rewards.");
    for (auto index : data.display[poolIndex].sources) {
        const auto& source = data.items[index];
        ImGui::PushID(index);
        if (ImGui::TreeNodeEx(
                "source", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", item_name(catalog, index))) {
            widgets::icon(widgets::find(catalog, index), 48);
            ImGui::TextWrapped("Item %u / 0x%08X", index, source.definitionHash);
            ImGui::TextWrapped((source.flags & 1) ? "Opens when acquired."
                                                  : "Kept as an item when acquired.");
            for (std::size_t i = 0; i < source.selectionCount; ++i) {
                const auto& selection = source.selections[i];
                ImGui::TextWrapped("Select up to %u rewards from category 0x%08X.",
                                   selection.count,
                                   selection.categoryHash);
                ImGui::TextDisabled("Selection policy %u", selection.policy);
            }
            if (source.acquiredFlag != rewards::kAbsent)
                ImGui::Text("Acquisition flag %u", source.acquiredFlag);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void pool_details(const Catalog& catalog, int index) {
    widgets::icon(pool_item(catalog, index), 56);
    ImGui::TextWrapped("%s", g_data->display[index].label.c_str());
    const auto& pool = g_data->pools[index];
    ImGui::TextWrapped(
        "Pool %d / 0x%08X / %u entries", index, pool.definitionHash, pool.entries.count);
    ImGui::SeparatorText("Source items");
    source_items(catalog, index);
}

void pool_tree(const Catalog& catalog, int poolIndex, std::size_t depth = 0) {
    if (depth >= rewards::kTraversalDepth) return;
    const auto& data = *g_data;
    const auto& pool = data.pools[poolIndex];
    for (std::size_t i = pool.entries.first; i < pool.entries.first + pool.entries.count; ++i) {
        const auto& entry = data.entries[i];
        const bool nested = entry.poolIndex < data.pools.size();
        const auto* item = widgets::find(catalog, entry.itemIndex);
        const char* label = nested ? data.display[entry.poolIndex].label.c_str()
                            : item ? widgets::name(item)
                                   : "Reward mapping";
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                     | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!nested) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (g_entry == static_cast<int>(i)) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushID(static_cast<int>(i));
        const bool open = tree_row("entry",
                                   flags,
                                   nested ? pool_item(catalog, entry.poolIndex) : item,
                                   label,
                                   entry.quantity);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_entry = static_cast<int>(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nQuantity %u / weight %g", label, entry.quantity, entry.weight);
        if (open && nested) {
            pool_tree(catalog, entry.poolIndex, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void pools(const Catalog& catalog) {
    const auto& data = *g_data;
    if (data.pools.empty()) return;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##table_search",
                             "Search package or reward names, hashes and indices",
                             g_search.data(),
                             g_search.size());
    if (!ImGui::BeginTable("##pool_browser", 3, ImGuiTableFlags_Resizable)) return;
    const auto& style = ImGui::GetStyle();
    const float iconSide = ImGui::GetFontSize() * 2.5f;
    ImGui::TableSetupColumn("Pools",
                            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                            iconSide + style.WindowPadding.x * 2 + style.ScrollbarSize);
    ImGui::TableSetupColumn("Rewards", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    // Bound both lists to leave room in the 64-icon cache for the inspector.
    const float listHeight =
        (std::min)(ImGui::GetContentRegionAvail().y,
                   18 * (iconSide + style.ItemSpacing.y) + style.WindowPadding.y * 2);
    ImGui::BeginChild("##pool_list", {0, listHeight}, ImGuiChildFlags_Borders);
    std::size_t found = 0;
    for (std::size_t i = 0; i < data.pools.size(); ++i) {
        const auto& display = data.display[i];
        if (!matches_text(display.search)) continue;
        ++found;
        ImGui::PushID(static_cast<int>(i));
        const auto origin = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##pool", g_pool == static_cast<int>(i), 0, {iconSide, iconSide}))
            select_pool(static_cast<int>(i));
        if (ImGui::IsItemVisible())
            reward_icon(pool_item(catalog, static_cast<int>(i)), origin, iconSide);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "%s\nPool %zu / 0x%08X", display.label.c_str(), i, data.pools[i].definitionHash);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    const float treeHeight =
        (std::min)(ImGui::GetContentRegionAvail().y,
                   36 * ImGui::GetFrameHeightWithSpacing() + style.WindowPadding.y * 2);
    ImGui::BeginChild("##pool_tree", {0, treeHeight}, ImGuiChildFlags_Borders);
    if (!found) ImGui::TextDisabled("No matching reward tables.");
    if (!g_history.empty() && ImGui::SmallButton("Back")) {
        g_pool = g_history.back();
        g_history.pop_back();
        g_entry = -1;
    }
    ImGui::PushID(g_pool);
    const bool open = tree_row("root",
                               ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow
                                   | ImGuiTreeNodeFlags_SpanAvailWidth
                                   | (g_entry < 0 ? ImGuiTreeNodeFlags_Selected : 0),
                               pool_item(catalog, g_pool),
                               data.display[g_pool].label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_entry = -1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", data.display[g_pool].label.c_str());
    if (open) {
        pool_tree(catalog, g_pool);
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("##pool_inspector", {0, 0}, ImGuiChildFlags_Borders);
    if (g_entry >= 0 && g_entry < static_cast<int>(data.entries.size())) {
        entry_details(catalog, data.entries[g_entry]);
    } else
        pool_details(catalog, g_pool);
    ImGui::EndChild();
    ImGui::EndTable();
}

void pass_details(const Catalog& catalog, int index) {
    const auto& row = g_data->pass[index];
    const auto* item = widgets::find(catalog, row.itemIndex);
    widgets::icon(item, 56);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", widgets::name(item));
    ImGui::Text("%s / Rank %u / Quantity %u",
                kPassTracks[premium(row) ? 1 : 0],
                row.requiredRank,
                row.quantity);
    ImGui::EndGroup();
    if (item && !item->description.empty()) ImGui::TextWrapped("%s", item->description.c_str());
    if (row.itemIndex < g_data->items.size()
        && g_data->items[row.itemIndex].poolIndex < g_data->pools.size()
        && ImGui::Button("View reward pool")) {
        select_pool(g_data->items[row.itemIndex].poolIndex);
        g_openPools = true;
    }
    if (ImGui::CollapsingHeader("Definition fields"))
        ImGui::TextWrapped("Reward row %d / Item %u / 0x%08X / Claim flag %u",
                           index,
                           row.itemIndex,
                           row.itemHash,
                           row.claimFlagIndex);
    if (ImGui::CollapsingHeader("Conditions"))
        condition(std::span(row.condition).first(row.conditionCount));
    if (row.socketCount && ImGui::CollapsingHeader("Socket overrides"))
        sockets(catalog, std::span(row.sockets).first(row.socketCount));
}

void pass(const Catalog& catalog) {
    const auto& data = *g_data;
    if (data.pass.empty()) {
        ImGui::TextDisabled("Season pass rewards are not available.");
        return;
    }
    const int lastRank = static_cast<int>(data.passRanks.size()) - 1;
    const int pages = (lastRank + kRanksPerPage - 1) / kRanksPerPage;
    g_passPage = (std::clamp)(g_passPage, 0, (std::max)(0, pages - 1));
    const int first = g_passPage * kRanksPerPage + 1;
    ImGui::Text("Ranks %d-%d / Page %d of %d",
                first,
                (std::min)(first + kRanksPerPage - 1, lastRank),
                g_passPage + 1,
                pages);
    int step = 0;
    const float navWidth = ImGui::GetFrameHeight();
    if (ImGui::BeginTable("##pass_page", 3)) {
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthFixed, navWidth);
        ImGui::TableSetupColumn("Ranks", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Next", ImGuiTableColumnFlags_WidthFixed, navWidth);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const float cardHeight = 76;
        const float pageHeight = ImGui::GetTextLineHeightWithSpacing()
                                 + 2 * (cardHeight + ImGui::GetStyle().CellPadding.y * 2);
        ImGui::BeginDisabled(g_passPage == 0);
        if (ImGui::Button("<##previous_page", {navWidth, pageHeight})) step = -1;
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        if (ImGui::BeginTable("##pass_rewards",
                              kRanksPerPage + 1,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn(
                "Track", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Premium").x + 8);
            for (int column = 0; column < kRanksPerPage; ++column)
                ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            for (int column = 0; column < kRanksPerPage; ++column) {
                ImGui::TableNextColumn();
                if (first + column <= lastRank) ImGui::Text("%d", first + column);
            }
            for (int track = 0; track < 2; ++track) {
                ImGui::TableNextRow();
                if (track == 1)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 130, 130, 50));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kPassTracks[track]);
                for (int column = 0; column < kRanksPerPage; ++column) {
                    ImGui::TableNextColumn();
                    const int rank = first + column;
                    if (rank > lastRank) continue;
                    const auto& rows = data.passRanks[rank][track];
                    const auto* row = rows.empty() ? nullptr : &data.pass[rows.front()];
                    const auto* item = row ? widgets::find(catalog, row->itemIndex) : nullptr;
                    const auto origin = ImGui::GetCursorScreenPos();
                    const ImVec2 size{ImGui::GetContentRegionAvail().x, cardHeight};
                    ImGui::PushID(rank * 2 + track);
                    ImGui::BeginDisabled(rows.empty());
                    if (widgets::card("##reward",
                                      {item, nullptr, row ? row->quantity : 0},
                                      g_passRank == rank && g_passTrack == track,
                                      size,
                                      false)) {
                        g_passRank = rank;
                        g_passTrack = track;
                        g_passReward = rows.front();
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::BeginTooltip();
                        ImGui::Text("%s / Rank %d", kPassTracks[track], rank);
                        if (rows.empty()) ImGui::TextDisabled("No reward at this rank.");
                        for (auto i : rows)
                            ImGui::Text("%s x%u",
                                        item_name(catalog, data.pass[i].itemIndex),
                                        data.pass[i].quantity);
                        ImGui::EndTooltip();
                    }
                    if (rows.size() > 1) {
                        char badge[32]{};
                        std::snprintf(badge, sizeof badge, "+%zu", rows.size() - 1);
                        ImGui::GetWindowDrawList()->AddText(
                            {origin.x, origin.y + size.y - ImGui::GetTextLineHeight()},
                            ImGui::GetColorU32(ImGuiCol_Text),
                            badge);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(g_passPage + 1 >= pages);
        if (ImGui::Button(">##next_page", {navWidth, pageHeight})) step = 1;
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (step != 0) {
        g_passPage += step;
        g_passRank = 0;
        g_passReward = -1;
    }
    ImGui::Separator();
    if (g_passRank == 0) {
        ImGui::TextDisabled("Select a reward to inspect its entries and class variants.");
        return;
    }
    if (ImGui::BeginTable("##pass_inspector", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Entries", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##pass_entries");
        ImGui::Text("%s / Rank %d", kPassTracks[g_passTrack], g_passRank);
        for (auto index : data.passRanks[g_passRank][g_passTrack]) {
            ImGui::PushID(index);
            const auto* item = widgets::find(catalog, data.pass[index].itemIndex);
            if (item_row("##reward_entry", item, widgets::name(item), g_passReward == index))
                g_passReward = index;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nReward row %d", widgets::name(item), index);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##pass_details");
        if (g_passReward >= 0) pass_details(catalog, g_passReward);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}
} // namespace
void draw(const Catalog& catalog) {
    if (!g_data && !load(catalog)) {
        ImGui::TextDisabled("Reward definitions are not available.");
        return;
    }
    ImGui::TextDisabled("%zu pools / %zu entries / %zu pass rows",
                        g_data->pools.size(),
                        g_data->entries.size(),
                        g_data->pass.size());
    if (ImGui::BeginTabBar("##reward_tables")) {
        const auto poolFlags = g_openPools ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        g_openPools = false;
        if (ImGui::BeginTabItem("Reward pools", nullptr, poolFlags)) {
            pools(catalog);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Season pass")) {
            pass(catalog);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
void clear() noexcept {
    g_data.reset();
    g_pool = 0;
    g_entry = -1;
    g_history.clear();
    g_openPools = false;
    g_passPage = g_passRank = g_passTrack = 0;
    g_passReward = -1;
}
} // namespace sunrise::client::ui::items::tables
