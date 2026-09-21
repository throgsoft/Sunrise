#pragma once

#include <cstdint>

#include "layout.h"

namespace sunrise::state {
struct PendingRecordRewardGrant;
}

namespace sunrise::middleware::datagen::family4::account {

/** Restores native empty receipt sentinels without changing any other account fields. */
void initialize_purchase_receipts(layout::Object& object) noexcept;

/**
 * Projects active State receipts plus an optional prepared purchase/Open/Refund after-image.
 * Receipt expiresAt is already absolute seconds in the projected client clock domain.
 * Leaves the object unchanged on lookup, identity, slot, or staged-before-image failure.
 * Reads State only; the encompassing transaction owns receipt persistence.
 */
[[nodiscard]] bool project_purchase_receipts(
    std::uint64_t accountSoid,
    layout::Object& object,
    const state::PendingRecordRewardGrant* mutation = nullptr) noexcept;

} // namespace sunrise::middleware::datagen::family4::account
