#pragma once

namespace sunrise::client::console {

/** Adds commands backed by current upstream State and client settings APIs. */
[[nodiscard]] bool install_commands() noexcept;

} // namespace sunrise::client::console
