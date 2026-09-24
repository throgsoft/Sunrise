#include <limits>

#include "../../encoding/bit_reader.h"
#include "opcode405.h"

namespace sunrise::middleware::web_service::messages::opcode405 {
/** Reads the biased signed fields and rejects trailing data. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    // 136 descriptor bits, two absent envelope fields and six alignment bits.
    constexpr std::size_t kPayloadBytes = 18;
    if (message.opcode != kOpcode || message.payload.size() != kPayloadBytes) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t vendor{};
    std::uint64_t bucket{};
    std::uint64_t instance{};
    std::uint64_t definition{};
    std::uint64_t quantity{};
    std::uint64_t tail{};
    if (!reader.read(16, vendor) || !reader.read(8, bucket) || !reader.read(64, instance)
        || !reader.read(16, definition) || !reader.read(32, quantity) || !reader.read(8, tail)
        || tail != 0 || reader.remaining_bits() != 0) {
        return false;
    }
    request = {static_cast<std::int16_t>(static_cast<std::int32_t>(vendor)
                                         + (std::numeric_limits<std::int16_t>::min)()),
               static_cast<std::int8_t>(static_cast<std::int16_t>(bucket)
                                        + (std::numeric_limits<std::int8_t>::min)()),
               instance,
               static_cast<std::int16_t>(static_cast<std::int32_t>(definition)
                                         + (std::numeric_limits<std::int16_t>::min)()),
               static_cast<std::int32_t>(static_cast<std::int64_t>(quantity)
                                         + (std::numeric_limits<std::int32_t>::min)())};
    return true;
}
} // namespace sunrise::middleware::web_service::messages::opcode405
