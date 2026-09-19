
#include "../../encoding/bit_reader.h"
#include "opcode402.h"

namespace sunrise::middleware::web_service::messages::opcode402 {

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t identity{}, instance{}, definition{}, value{}, selector{}, padding{};
    if (!reader.read(1, identity) || !reader.read(64, instance) || !reader.read(16, definition)
        || !reader.read(32, value) || !reader.read(8, selector) || !reader.read(7, padding)
        || padding != 0 || reader.remaining_bits() != 0) {
        return false;
    }
    request = {identity != 0,
               instance,
               static_cast<std::int16_t>(static_cast<std::int32_t>(definition) - 32768),
               static_cast<std::int32_t>(static_cast<std::int64_t>(value) - 2147483648LL),
               static_cast<std::int8_t>(static_cast<std::int16_t>(selector) - 128)};
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode402
