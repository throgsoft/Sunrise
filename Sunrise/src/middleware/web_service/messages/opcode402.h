#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode402 {

inline constexpr std::uint16_t kOpcode = 402;
inline constexpr std::size_t kPayloadSize = 16;
/** Policy of the supported Character inventory action, not a property of the wire codec. */
inline constexpr std::uint32_t kDiscardQuantity = 1;

/** The 128-bit descriptor: 121 fixed bits plus seven trailing padding bits.
 * The identity boolean does not omit the SOID, and the signed definition, value and selector
 * fields carry no presence guards. Unsupported action forms are preserved for the dispatcher.
 */
struct Request {
    bool hasInstance{};
    std::uint64_t instanceSoid{};
    std::int16_t definitionIndex{};
    /** Stack quantity the Character action reports. */
    std::int32_t value{};
    /** Signed UI selector; the hold action can send -1. */
    std::int8_t selector{};
};

/** Decodes the fixed descriptor; inventory action validation belongs to the caller.
 * A failed parse clears request.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

/** The Character action only. Profile or negative-selector forms must not silently become
 * this one-unit mutation without their own producer and ownership semantics.
 */
[[nodiscard]] constexpr bool supported_character_action(const Request& request) noexcept {
    return request.hasInstance && request.instanceSoid != 0 && request.definitionIndex >= 0
           && request.value > 0 && request.selector >= 0;
}

} // namespace sunrise::middleware::web_service::messages::opcode402
