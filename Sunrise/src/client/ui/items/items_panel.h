#pragma once

namespace sunrise::client::ui::items {
void draw() noexcept;
/** Renderer teardown stops package work before installed decompression/target teardown. */
void release_renderer() noexcept;
void shutdown() noexcept;
} // namespace sunrise::client::ui::items
