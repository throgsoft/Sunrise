#include <algorithm>
#include <array>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "opcode905.h"

namespace sunrise::middleware::web_service::messages::opcode905 {

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode
        || (message.payload.size() != kMinimumPayloadSize
            && message.payload.size() != kMaximumPayloadSize))
        return false;

    encoding::bits::Reader reader(message.payload);
    Request candidate{};
    std::uint64_t mode{}, definition{}, clockPresent{}, trailer{};
    if (!reader.read(2, mode) || !reader.read(64, candidate.instanceSoid)
        || !reader.read(16, definition) || !reader.read(1, clockPresent)
        || (clockPresent != 0 && !reader.read(64, candidate.clock))
        || !reader.read(5, trailer) || trailer != 0 || reader.remaining_bits() != 0)
        return false;

    candidate.mode = static_cast<std::int8_t>(static_cast<std::int16_t>(mode) - 1);
    candidate.definitionIndex =
        static_cast<std::int16_t>(static_cast<std::int32_t>(definition) - 32768);
    candidate.hasClock = clockPresent != 0;
    request = candidate;
    return true;
}

bool encode_request(const Request& request,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    const auto payloadSize = request.hasClock ? kMaximumPayloadSize : kMinimumPayloadSize;
    if (request.mode < -1 || request.mode > 2 || (!request.hasClock && request.clock != 0)
        || output.size() < payloadSize)
        return false;

    std::array<std::byte, kMaximumPayloadSize> staged{};
    encoding::bits::Writer writer(std::span<std::byte>{staged}.first(payloadSize));
    std::size_t size{};
    // Type 6 is the MSB-first integer codec, not type 12's raw memory representation.
    if (!writer.write(static_cast<std::uint16_t>(static_cast<std::int16_t>(request.mode) + 1), 2)
        || !writer.write(request.instanceSoid, 64)
        || !writer.write(
            static_cast<std::uint32_t>(static_cast<std::int32_t>(request.definitionIndex) + 32768),
            16)
        || !writer.write(request.hasClock ? 1 : 0, 1)
        || (request.hasClock && !writer.write(request.clock, 64))
        || !writer.write(0, 5) || !writer.finish(size) || size != payloadSize)
        return false;

    std::copy_n(staged.begin(), size, output.begin());
    written = size;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode905
