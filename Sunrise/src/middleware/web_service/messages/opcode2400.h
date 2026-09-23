#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode2400 {

/** Web Service opcode used to claim one reward from a progression reward list. */
inline constexpr std::uint16_t kOpcode = 2400;
/** Envelope header followed by two biased indices and one padding byte. */
inline constexpr std::size_t kRequestSize = kEnvelopeHeaderSize + 5;

/** Exact pair carried by the native fixed two-element request array. */
struct Request {
    std::uint16_t progressionIndex{};
    std::uint16_t rewardIndex{};
};

/** Parses the complete native opcode-2400 progression/reward pair. */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

/** Encodes one complete native progression claim, including its envelope. */
[[nodiscard]] bool encode_request(const Request& request,
                                  std::uint32_t transactionId,
                                  std::span<std::byte> output,
                                  std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode2400
