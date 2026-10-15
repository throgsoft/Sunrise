#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "../../encoding/byte_order.h"
#include "biased_field.h"
#include "opcode2400.h"

namespace sunrise::middleware::web_service::messages::opcode2400 {
namespace {

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

/**
 * Writes the byte-aligned envelope header, then the biased pair parse_request reads back.
 * @return False when the output is short or an index does not fit the signed biased field.
 */
bool encode_request(const Request& request,
                    std::uint32_t transactionId,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kRequestSize || request.progressionIndex >= kBiasedIndexBias
        || request.rewardIndex >= kBiasedIndexBias) {
        return false;
    }
    encoding::write_u16_be(output.first<encoding::kU16Size>(), kOpcode);
    encoding::write_u32_be(output.subspan<encoding::kU16Size, encoding::kU32Size>(), transactionId);
    encoding::bits::Writer writer(output.subspan(kEnvelopeHeaderSize, kPayloadSize));
    std::size_t payloadSize = 0;
    if (!writer.write(static_cast<std::uint32_t>(request.progressionIndex + kBiasedIndexBias),
                      kBiasedIndexWidth)
        || !writer.write(static_cast<std::uint32_t>(request.rewardIndex + kBiasedIndexBias),
                         kBiasedIndexWidth)
        || !writer.write(0U, kPaddingWidth) || !writer.finish(payloadSize)
        || payloadSize != kPayloadSize) {
        return false;
    }
    written = kRequestSize;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode2400
