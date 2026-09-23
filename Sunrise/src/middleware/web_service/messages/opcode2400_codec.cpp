#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "biased_field.h"
#include "opcode2400.h"

namespace sunrise::middleware::web_service::messages::opcode2400 {
namespace {

/** Two 16-bit array elements plus the descriptor's final zero padding byte. */
constexpr std::size_t kPayloadSize = kRequestSize - kEnvelopeHeaderSize;
/** The descriptor pads its four payload bytes out to five. */
constexpr std::uint8_t kPaddingWidth = 8;

} // namespace

/**
 * Parses the biased progression and reward pair carried by opcode 2400.
 * @param request Receives both indexes; unchanged content on failure.
 * @return False on a wrong opcode, a wrong payload size, non-zero padding, or a negative index.
 */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::int16_t progression = 0;
    std::int16_t reward = 0;
    std::uint64_t padding = 0;
    if (!read_biased_index(reader, progression) || !read_biased_index(reader, reward)
        || !reader.read(kPaddingWidth, padding) || reader.remaining_bits() != 0 || padding != 0
        || progression < 0 || reward < 0) {
        return false;
    }
    request.progressionIndex = static_cast<std::uint16_t>(progression);
    request.rewardIndex = static_cast<std::uint16_t>(reward);
    return true;
}

bool encode_request(const Request& request,
                    std::uint32_t transactionId,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kRequestSize || request.progressionIndex >= kBiasedIndexBias
        || request.rewardIndex >= kBiasedIndexBias) {
        return false;
    }
    encoding::bits::Writer writer(output.first(kRequestSize));
    return writer.write(kOpcode, 16) && writer.write(transactionId, 32)
           && writer.write(request.progressionIndex + kBiasedIndexBias, kBiasedIndexWidth)
           && writer.write(request.rewardIndex + kBiasedIndexBias, kBiasedIndexWidth)
           && writer.write(0, kPaddingWidth) && writer.finish(written);
}

} // namespace sunrise::middleware::web_service::messages::opcode2400
