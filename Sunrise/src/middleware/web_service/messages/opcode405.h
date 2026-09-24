#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode405 {
inline constexpr std::uint16_t kOpcode = 405;
/** Vendor-mediated transfer. The vendor rule, not the request, selects the destination. */
struct Request {
    std::int16_t vendorIndex{};
    std::int8_t sourceBucket{};
    std::uint64_t instanceSoid{};
    std::int16_t definitionIndex{};
    std::int32_t quantity{};
};
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;
} // namespace sunrise::middleware::web_service::messages::opcode405
