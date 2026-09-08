#pragma once

namespace sunrise::state {
enum class DawningOvenBootstrapStatus { ready, notReady, refused };
struct DawningOvenBootstrapResult {
    DawningOvenBootstrapStatus status{DawningOvenBootstrapStatus::notReady};
    bool changed{};
};

/** Grants the installed default oven once per character. SQLite commits the marker with the
 * item; a later discard does not cause automatic reacquisition. Existing oven state is kept.
 * Call after content readiness, before the initial account snapshot and character selection. */
[[nodiscard]] DawningOvenBootstrapResult ensure_default_dawning_oven() noexcept;
} // namespace sunrise::state
