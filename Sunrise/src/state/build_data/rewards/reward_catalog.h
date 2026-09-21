#pragma once

#include "definition.h"

namespace sunrise::state::build_data::rewards {

void clear() noexcept;
[[nodiscard]] bool ready() noexcept;
[[nodiscard]] bool valid(View data) noexcept;
[[nodiscard]] bool replace(View data) noexcept;
/** The borrowed view remains valid only during the callback. */
[[nodiscard]] bool read(void* context, bool (*consume)(void*, View) noexcept) noexcept;
[[nodiscard]] bool find_item(std::uint16_t itemIndex, Item& item) noexcept;

[[nodiscard]] bool snapshot(std::span<Pool> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Entry> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Item> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Instruction> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Modifier> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<SocketOverride> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::rewards
