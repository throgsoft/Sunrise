#include "items_widgets.h"

#include <algorithm>
#include <cstdio>
#include <span>

#include "items_icon_cache.h"

namespace sunrise::client::ui::items::widgets {
const Entry* find(const Catalog& catalog, std::uint16_t index) noexcept {
    const auto found = std::lower_bound(catalog.entries.begin(),
                                        catalog.entries.end(),
                                        index,
                                        [](const Entry& entry, std::uint16_t value) {
                                            return entry.identity.definitionIndex < value;
                                        });
    return found != catalog.entries.end() && found->identity.definitionIndex == index ? &*found
                                                                                      : nullptr;
}

const char* name(const Entry* entry) noexcept {
    return entry && !entry->name.empty() ? entry->name.c_str() : "Unnamed item";
}

void icon(const Entry* entry, float side) {
    const auto texture = entry && ImGui::IsRectVisible({side, side}) ? icons::get(entry->iconIndex)
                                                                     : ImTextureID_Invalid;
    if (texture != ImTextureID_Invalid) {
        ImGui::Image(ImTextureRef(texture), {side, side});
    } else {
        ImGui::Dummy({side, side});
    }
}

void image(ImTextureID texture, ImVec2 minimum, ImVec2 maximum) {
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(texture),
                                         minimum,
                                         maximum,
                                         {0, 0},
                                         {1, 1},
                                         ImGui::GetColorU32(IM_COL32_WHITE));
}

bool card(const char* id, const Card& card, bool selected, ImVec2 size, bool showName) {
    const auto& style = ImGui::GetStyle();
    const float width = size.x, height = size.y;
    const float side =
        (std::min)({64.0f,
                    width,
                    showName ? height - ImGui::GetTextLineHeight() * 2 - style.ItemSpacing.y
                             : height});
    const auto* label = card.label ? card.label : name(card.item);
    const auto origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable(id, selected, 0, size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start{origin.x + (width - side) * 0.5f, origin.y};
    const ImVec2 end{start.x + side, start.y + side};
    const auto texture = card.item ? icons::get(card.item->iconIndex) : ImTextureID_Invalid;
    if (texture != ImTextureID_Invalid) {
        image(texture, start, end);
    } else {
        draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
    }
    if (selected || card.equipped) {
        draw->AddRect(
            start, end, ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_Text), 0, 0, 2);
    }
    if (card.quantity > 1) {
        char count[24]{};
        std::snprintf(count, sizeof count, "%u", card.quantity);
        const auto textSize = ImGui::CalcTextSize(count);
        const ImVec2 pos{end.x - textSize.x - 3, end.y - textSize.y};
        draw->AddRectFilled({pos.x - 2, pos.y}, end, ImGui::GetColorU32(IM_COL32(0, 0, 0, 210)));
        draw->AddText(pos, ImGui::GetColorU32(IM_COL32_WHITE), count);
    }
    if (showName) {
        const ImVec2 text{origin.x + 2, end.y + style.ItemSpacing.y};
        draw->PushClipRect(text, {origin.x + width - 2, origin.y + height}, true);
        draw->AddText(ImGui::GetFont(),
                      ImGui::GetFontSize(),
                      text,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      label,
                      nullptr,
                      width - 4);
        draw->PopClipRect();
    }
    return clicked;
}

int grid(const char* id, std::span<const Card> cards, int selected, float maxHeight) {
    constexpr float width = 104, side = 64;
    const auto& style = ImGui::GetStyle();
    const auto available = ImGui::GetContentRegionAvail();
    const int columns = (std::clamp)(static_cast<int>((available.x - style.WindowPadding.x * 2
                                                       - style.ScrollbarSize)
                                                      / (width + style.ItemSpacing.x)),
                                     1,
                                     12);
    const float height = side + ImGui::GetTextLineHeight() * 2 + style.ItemSpacing.y;
    const float pitch = height + style.ItemSpacing.y;
    // Leave cache space for source icons, details, and a clipped row.
    constexpr int gridIcons = icons::kCapacity - 16;
    float viewport =
        (std::min)(available.y,
                   static_cast<float>(gridIcons / columns - 1) * pitch + style.WindowPadding.y * 2);
    if (maxHeight > 0) {
        viewport = (std::min)(viewport, maxHeight);
    }
    ImGui::BeginChild(id, {0, (std::max)(1.0f, viewport)}, ImGuiChildFlags_Borders);
    if (cards.empty()) {
        ImGui::TextDisabled("No items.");
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>((cards.size() + columns - 1) / columns), pitch);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < columns; ++col) {
                const auto i = row * columns + col;
                if (i >= static_cast<int>(cards.size())) {
                    break;
                }
                if (col) {
                    ImGui::SameLine();
                }
                const auto& value = cards[i];
                ImGui::PushID(i);
                if (card("##card", value, selected == i, {width, height})) {
                    selected = i;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(value.label ? value.label : name(value.item));
                    if (value.item) {
                        ImGui::TextDisabled("Item %u / 0x%08X",
                                            value.item->identity.definitionIndex,
                                            value.item->identity.definitionHash);
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    return selected;
}
} // namespace sunrise::client::ui::items::widgets
