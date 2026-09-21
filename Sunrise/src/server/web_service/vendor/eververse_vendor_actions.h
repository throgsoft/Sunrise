#pragma once

#include <cstdint>

namespace sunrise::middleware::web_service {
struct Message;
}

namespace sunrise::middleware::web_service::messages::opcode402 {
struct Request;
}

namespace sunrise::server::web_service {
struct Outcome;
namespace vendor {
/** Claims wrapper Open and cosmetic Unlock actions, including refused or stale sources. */
[[nodiscard]] bool intercept_cosmetic_unlock(
    const middleware::web_service::messages::opcode402::Request& request,
    Outcome& outcome) noexcept;
/** Prepares only mode-1 opcode-905 refunds backed by an authoritative package receipt. */
void refund_package(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
/** Claims every Store request, including refusals, so none reaches Collections pricing. */
[[nodiscard]] bool intercept_eververse_purchase(std::uint16_t opcode,
                                                std::int32_t vendorIndex,
                                                std::int32_t saleIndex,
                                                std::uint16_t itemDefinitionIndex,
                                                Outcome& outcome) noexcept;
} // namespace vendor
} // namespace sunrise::server::web_service
