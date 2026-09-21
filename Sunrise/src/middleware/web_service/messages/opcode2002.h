#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode2002 {
inline constexpr std::uint16_t kOpcode = 2002;

struct Request {
    std::uint64_t instanceSoid{};
};

/** Reflected 808075F8: 64-bit SOID, then two absent blob flags and six pad bits. */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;
} // namespace sunrise::middleware::web_service::messages::opcode2002
