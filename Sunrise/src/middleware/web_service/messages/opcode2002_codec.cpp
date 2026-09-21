#include "../../encoding/bit_reader.h"
#include "opcode2002.h"

namespace sunrise::middleware::web_service::messages::opcode2002 {
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != 9) return false;

    encoding::bits::Reader reader(message.payload);
    Request candidate{};
    std::uint64_t trailer{};
    // DFE410 appends two absent blob flags after the reflected SOID; finalization
    // adds six zero pad bits. Message.payload retains this envelope trailer.
    if (!reader.read(64, candidate.instanceSoid)
        || !reader.read(8, trailer) || trailer != 0) {
        return false;
    }
    request = candidate;
    return true;
}
} // namespace sunrise::middleware::web_service::messages::opcode2002
