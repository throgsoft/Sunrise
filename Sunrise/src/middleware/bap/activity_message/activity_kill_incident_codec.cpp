#include "../../encoding/bit_reader.h"
#include "kill_incident.h"

namespace sunrise::middleware::bap::activity_message::kill_incident {
namespace {
using encoding::bits::Reader;
bool optional_skip(Reader& reader, std::size_t width) noexcept {
    std::uint64_t present{};
    return reader.read(1, present) && (!present || reader.skip(width));
}
template <typename T>
bool optional_value(Reader& reader, std::uint8_t width, std::optional<T>& output) noexcept {
    std::uint64_t present{}, value{};
    if (!reader.read(1, present)) return false;
    if (present) {
        if (!reader.read(width, value)) return false;
        output = static_cast<T>(value);
    }
    return true;
}
bool actor(Reader& reader, Actor& output) noexcept {
    std::uint64_t combatant{};
    if (!reader.read(1, combatant) || !reader.skip(2)
        || !optional_value(reader, 32, output.classHash) || !reader.skip(3 + 2 + 6)
        || !optional_skip(reader, 32) || !optional_skip(reader, 32)
        || !optional_value(reader, 64, output.player) || !optional_skip(reader, 64)
        || !optional_skip(reader, 64) || !optional_skip(reader, 32))
        return false;
    output.combatant = combatant != 0;
    return true;
}
bool labels(Reader& reader, Labels& output) noexcept {
    std::uint64_t count{}, hash{};
    if (!reader.read(4, count) || count > output.values.size()) return false;
    output.count = static_cast<std::size_t>(count);
    for (std::size_t i = 0; i < output.count; ++i) {
        if (!reader.read(32, hash) || hash == 0) return false;
        output.values[i] = static_cast<std::uint32_t>(hash);
        for (std::size_t j = 0; j < i; ++j)
            if (output.values[j] == output.values[i]) return false;
    }
    return true;
}
} // namespace
bool decode_raw(std::span<const std::byte> bytes, Payload& output) noexcept {
    output = {};
    Payload candidate{};
    Reader reader(bytes);
    std::uint64_t sequence{}, target{}, affinity{}, count{}, padding{};
    // Routing, common incident fields and the uncompressed XYZ vector.
    if (!reader.read(32, sequence) || !reader.read(3, target)
        || !reader.read(64, candidate.recipient) || !optional_skip(reader, 5)
        || !optional_skip(reader, 7) || !reader.skip(96) || !optional_skip(reader, 11)
        || !optional_skip(reader, 32) || !reader.skip(11 + 32 + 32) || !reader.read(3, affinity)
        || !optional_skip(reader, 8 + 3) || !reader.skip(1 + 3 * 64)
        || !actor(reader, candidate.killer) || !actor(reader, candidate.victim)
        || !optional_skip(reader, 32) || !optional_skip(reader, 32) || !reader.read(4, count)
        || count > 9)
        return false;
    // The bounded assist records have no optional fields in this wire profile.
    for (std::uint64_t i = 0; i < count; ++i)
        if (!reader.skip(3 + 64 + 6 + 64 + 64 + 32 + 1 + 3 + 2 + 8)) return false;
    if (!reader.skip(32) || !labels(reader, candidate.source) || !labels(reader, candidate.actor)
        || !labels(reader, candidate.target) || !reader.skip(64) || reader.remaining_bits() > 7
        || !reader.read(static_cast<std::uint8_t>(reader.remaining_bits()), padding)
        || padding != 0)
        return false;
    candidate.reorderSequence = static_cast<std::uint32_t>(sequence);
    candidate.targetKind = static_cast<std::int32_t>(target) - 1;
    candidate.damage = static_cast<std::int32_t>(affinity) - 1;
    output = candidate;
    return true;
}
} // namespace sunrise::middleware::bap::activity_message::kill_incident
