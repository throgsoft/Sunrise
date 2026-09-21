#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::eververse {

inline constexpr std::size_t kRefundReceiptCapacity = 30;
/** Retail's unopened-item return period. This is server policy, not a client timer. */
inline constexpr std::int64_t kRefundPeriodSeconds = 7 * 24 * 60 * 60;

/** One paid, unopened package. Native table indices are resolved only when projecting it. */
struct PurchaseReceipt {
    std::uint64_t sourceInstanceSoid{};
    std::uint64_t characterSoid{};
    std::uint32_t wrapperHash{};
    std::uint32_t itemHash{};
    std::uint32_t currencyHash{};
    std::uint32_t cost{};
    std::uint16_t vendorIndex{};
    std::uint16_t saleIndex{};
    std::int64_t purchasedAt{};
    std::int64_t expiresAt{};
    std::uint8_t slot{};
    bool operator==(const PurchaseReceipt&) const = default;
};

enum class PackageAction : std::uint8_t { open, refund };

/** Reads only unopened receipts, preserving their stable native slots. */
[[nodiscard]] bool read_purchase_receipts(
    std::uint64_t accountSoid,
    std::array<PurchaseReceipt, kRefundReceiptCapacity>& receipts) noexcept;

/** A package SOID is never reused, including after Open/Refund or a developer inventory clear. */
[[nodiscard]] bool reserve_purchase_identity(std::uint64_t accountSoid,
                                             std::uint64_t minimum,
                                             std::uint64_t& soid,
                                             std::uint8_t& slot) noexcept;
/** These writes compose with the encompassing inventory/wallet transaction. */
[[nodiscard]] bool insert_purchase_receipt(std::uint64_t accountSoid,
                                           const PurchaseReceipt& receipt) noexcept;
[[nodiscard]] bool consume_purchase_receipt(std::uint64_t accountSoid,
                                            const PurchaseReceipt& before,
                                            PackageAction action) noexcept;

} // namespace sunrise::state::eververse
