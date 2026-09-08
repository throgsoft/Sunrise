#pragma once

namespace sunrise::client::console {
/** Initializes the independent developer console and its command registry. */
[[nodiscard]] bool initialize() noexcept;
/** Home toggles this window independently of the main Sunrise surface. */
void toggle() noexcept;
[[nodiscard]] bool open() noexcept;
/** Shared input policy for the existing renderer, cursor, raw input and movement hooks. */
[[nodiscard]] bool captures_input() noexcept;
/** Draws the dirty/quest console window inside the existing ImGui frame. */
[[nodiscard]] bool draw() noexcept;
/** Call after rendering/input hooks quiesce. */
void shutdown() noexcept;
} // namespace sunrise::client::console
