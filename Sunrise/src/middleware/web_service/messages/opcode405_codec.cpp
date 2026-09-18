#include "../../encoding/bit_reader.h"
#include "opcode405.h"

namespace sunrise::middleware::web_service::messages::opcode405 {
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    // 136 descriptor bits, two absent envelope fields and six alignment bits.
    if (message.opcode != kOpcode || message.payload.size() != 18) return false;
    encoding::bits::Reader reader(message.payload);
    std::uint64_t vendor{}, bucket{}, instance{}, definition{}, quantity{}, tail{};
    if (!reader.read(16, vendor) || !reader.read(8, bucket) || !reader.read(64, instance)
        || !reader.read(16, definition) || !reader.read(32, quantity) || !reader.read(8, tail)
        || tail != 0 || reader.remaining_bits() != 0)
        return false;
    request = {static_cast<std::int16_t>(static_cast<std::int32_t>(vendor) - 32768),
               static_cast<std::int8_t>(static_cast<std::int16_t>(bucket) - 128),
               instance,
               static_cast<std::int16_t>(static_cast<std::int32_t>(definition) - 32768),
               static_cast<std::int32_t>(static_cast<std::int64_t>(quantity) - 2147483648LL)};
    return true;
}
} // namespace sunrise::middleware::web_service::messages::opcode405
