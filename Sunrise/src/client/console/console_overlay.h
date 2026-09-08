#pragma once

namespace sunrise::client::console {

/** Registers Client > Console with the existing upstream UI; call after State and Core UI start. */
[[nodiscard]] bool initialize() noexcept;

/** Unregisters the page. Call after rendering is quiesced and before State shuts down. */
void shutdown() noexcept;

} // namespace sunrise::client::console
