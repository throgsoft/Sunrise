#pragma once

namespace sunrise::state {
enum class DefaultInventoryBootstrapStatus { ready, notReady, refused };
struct DefaultInventoryBootstrapResult {
    DefaultInventoryBootstrapStatus status{DefaultInventoryBootstrapStatus::notReady};
    bool changed{};
};

/** Grants the installed default oven once per character. SQLite commits the marker with the
 * item; a later discard does not cause automatic reacquisition. Existing oven state is kept.
 * Call after content readiness, before the initial account snapshot and character selection. */
[[nodiscard]] DefaultInventoryBootstrapResult ensure_default_dawning_oven() noexcept;

/** Grants the installed Weak Synthesizer and Chalice once per character before publication.
 * Keeps existing items, sockets and earned tiers. Each grant and marker commit together. */
[[nodiscard]] DefaultInventoryBootstrapResult ensure_default_activity_containers() noexcept;

} // namespace sunrise::state
