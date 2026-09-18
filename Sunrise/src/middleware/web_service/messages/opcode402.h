#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode402 {

inline constexpr std::uint16_t kOpcode = 402;
inline constexpr std::size_t kPayloadSize = 16;
/** Policy of the supported Character inventory action, not a property of the wire codec. */
inline constexpr std::uint32_t kDiscardQuantity = 1;

/** Native 8080761F/80807622: 121 fixed bits plus seven trailing padding bits.
 * The identity boolean does not omit the SOID. Signed definition/value/selector fields have
 * no presence guards. Preserve unsupported action forms for the semantic dispatcher to reject.
 */
struct Request {
    bool hasInstance{};
    std::uint64_t instanceSoid{};
    std::int16_t definitionIndex{};
    /** Observed stack quantity in the supported action; other UI producers remain under RE. */
    std::int32_t value{};
    /** Signed UI selector. The native hold-action producer can send -1. */
    std::int8_t selector{};
    friend constexpr bool operator==(const Request&, const Request&) = default;
};

/** Complete fixed descriptor codec; inventory action validation belongs to the caller.
 * Decode failure clears request. Encoding failure clears written and preserves output.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;
[[nodiscard]] bool
encode_request(const Request& request, std::span<std::byte> output, std::size_t& written) noexcept;

/** Existing tested Character action only. Decodable profile or negative-selector forms must
 * not silently become this one-unit mutation without their producer/ownership semantics.
 */
[[nodiscard]] constexpr bool supported_character_action(const Request& request) noexcept {
    return request.hasInstance && request.instanceSoid != 0 && request.definitionIndex >= 0
           && request.value > 0 && request.selector >= 0;
}

} // namespace sunrise::middleware::web_service::messages::opcode402
