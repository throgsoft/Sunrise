#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode905 {

inline constexpr std::uint16_t kOpcode = 905;
inline constexpr std::size_t kMinimumPayloadSize = 11;
inline constexpr std::size_t kMaximumPayloadSize = 19;

/** Native refund action 2, descriptor 808075D8. Mode 1 names a held source;
 * mode 2 comes from other source contexts. State supports receipt-backed held packages only.
 */
struct Request {
    /** Signed native enum: the two wire bits encode mode + 1, covering -1 through 2. */
    std::int8_t mode{-1};
    std::uint64_t instanceSoid{};
    std::int16_t definitionIndex{-1};
    /** Presence is independent of the clock value, including a present zero. */
    bool hasClock{};
    std::uint64_t clock{};
    friend constexpr bool operator==(const Request&, const Request&) = default;
};

/** Parses 83/147 descriptor bits, two absent envelope blobs, and three zero pad bits.
 * Failure clears request. Unsupported semantic modes remain available to the dispatcher.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

/** Encodes canonical requests. An absent clock must have value zero.
 * Failure clears written and preserves output.
 */
[[nodiscard]] bool
encode_request(const Request& request, std::span<std::byte> output, std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode905
