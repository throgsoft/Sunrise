#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::server::bap {

enum class DeveloperGrantStatus : std::uint8_t {
    unavailable,
    queued,
    publishing,
    published,
    unchanged,
    refused
};

/** Published means committed and copied to the normal BAP response, not acknowledged by UI. */
struct DeveloperGrantReceipt {
    std::uint64_t id{};
    DeveloperGrantStatus status{DeveloperGrantStatus::unavailable};
    std::size_t changed{};
    std::size_t inventoryCount{};
    std::size_t postmasterCount{};
    std::size_t materialCount{};
    std::int32_t family4Version{};
    const char* reason{"receipt unavailable"};
};

/** Captures the selected State identity and queues only; never enters the BAP transport lock
 * or pump. At most eight pending requests, nine instanced copies per request, two-minute TTL.
 * Pending developer commands are process-local and never commit without a matching F4 peer.
 */
[[nodiscard]] DeveloperGrantReceipt enqueue_developer_item_grant(
    std::uint16_t index, std::int32_t quantity, std::uint32_t expectedHash) noexcept;
/** Lock-local receipt lookup; eight slots retain pending work and the newest finished receipts. */
[[nodiscard]] DeveloperGrantReceipt developer_item_grant_receipt(std::uint64_t id) noexcept;

} // namespace sunrise::server::bap
