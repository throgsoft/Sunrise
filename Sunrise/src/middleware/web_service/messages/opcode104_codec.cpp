#include "../../encoding/bit_reader.h"
#include "opcode104.h"

namespace sunrise::middleware::web_service::messages::opcode104 {
bool parse_request(const Message& message, Request& output) noexcept {
    output = {};
    if (message.opcode != kOpcode || message.payload.size() != 1) return false;
    encoding::bits::Reader reader(message.payload);
    std::uint64_t first{}, second{}, trailer{};
    // Two field bits, two absent envelope-blob flags, then four zero padding bits.
    if (!reader.read(1, first) || !reader.read(1, second)
        || !reader.read(6, trailer) || trailer != 0)
        return false;
    output = {first != 0, second != 0};
    return true;
}
} // namespace sunrise::middleware::web_service::messages::opcode104
