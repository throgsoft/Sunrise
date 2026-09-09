#pragma once

namespace sunrise::client::hooks::season_xp_toast {

/** Attach both optional presentation hooks together; a signature miss leaves both detached. */
[[nodiscard]] bool install() noexcept;
/** False means protected native calls are still active: retain the module and retry teardown. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::season_xp_toast
