#pragma once

namespace sunrise::client::ui::runtime {

/** @return True when the Client module owns its Core UI registry slot. */
[[nodiscard]] bool initialize() noexcept;

/** Releases module-owned GPU resources while the renderer holds its lifecycle lock. */
void release_renderer() noexcept;

/** Removes the Client module from the Core UI registry. */
void shutdown() noexcept;

} // namespace sunrise::client::ui::runtime
