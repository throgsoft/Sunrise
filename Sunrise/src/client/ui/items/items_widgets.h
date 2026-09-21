#pragma once

#include <imgui.h>
#include <span>

#include "items_catalog.h"

namespace sunrise::client::ui::items::widgets {
const Entry* find(const Catalog& catalog, std::uint16_t index) noexcept;
const char* name(const Entry* entry) noexcept;
void icon(const Entry* entry, float side);

struct Card {
    const Entry* item{};
    const char* label{};
    std::uint32_t quantity{};
    bool equipped{};
};

bool card(const char* id, const Card& card, bool selected, ImVec2 size, bool showName = true);
int grid(const char* id, std::span<const Card> cards, int selected, float maxHeight = 0);
} // namespace sunrise::client::ui::items::widgets
