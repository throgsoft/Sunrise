#pragma once

#include <cstdint>

namespace sunrise::client::material_toast {

enum class Status { ready, refused, delivered };
struct Result {
    Status status{Status::refused};
    const char* reason{"not_initialized"};
};

/** Resolve once from the Steam callback's client/game-thread section. Never call from State.
 * Binds the adapter to that thread. Does not display anything or touch inventory/counters. */
[[nodiscard]] Result initialize() noexcept;

/** Thread-only queue admission. Does not bind an idle caller or enter native code.
 * True before the first initialize(); thereafter true only on its bound thread. */
[[nodiscard]] bool may_service_current_thread() noexcept;

/** Check readiness for a State Dawning ingredient ordinal, resolving its installed pickup.
 * Must run on the same game thread as initialize(). */
[[nodiscard]] Result ready(std::uint8_t ingredientOrdinal) noexcept;

/** Present an already committed positive wallet gain. Caller owns commit ordering/deduplication.
 * No inventory entry, account value, or persistence is changed. `delivered` means the native
 * notification queue copied the requested identity/quantity, not that the HUD rendered it.
 * A dispatch_fault/dispatch_unconfirmed permanently quarantines this adapter; do not retry
 * that event because the native call may have partly executed. */
[[nodiscard]] Result present(std::uint8_t ingredientOrdinal, std::int32_t committedGain) noexcept;

} // namespace sunrise::client::material_toast
