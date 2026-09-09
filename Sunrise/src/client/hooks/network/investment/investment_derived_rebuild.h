#pragma once

namespace sunrise::client::hooks::network::investment {

/** Arms one derived-state rebuild on a committed investment publication. */
void notify_investment_publication() noexcept;

/** Advances cache invalidation and arms freshness after a native definition expression patch. */
void notify_investment_definition_change() noexcept;

/** @return True when freshness and real-arrival arms are attached; cache refresh is optional. */
[[nodiscard]] bool install() noexcept;

/** @return True when every investment rebuild detour is absent. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while required freshness and real-arrival hooks are attached. */
[[nodiscard]] bool is_installed() noexcept;

/** @return True while any investment rebuild detour still needs cleanup. */
[[nodiscard]] bool has_ownership() noexcept;

} // namespace sunrise::client::hooks::network::investment
