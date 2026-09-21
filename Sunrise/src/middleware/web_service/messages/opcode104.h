#pragma once

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode104 {
inline constexpr std::uint16_t kOpcode = 104;

// Native 8080770D carries two booleans. Their individual policy meanings are not decoded.
struct Request {
    bool first{};
    bool second{};
};

[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;
} // namespace sunrise::middleware::web_service::messages::opcode104
