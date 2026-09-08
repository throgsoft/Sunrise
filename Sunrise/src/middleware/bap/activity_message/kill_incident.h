#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sunrise::middleware::bap::activity_message::kill_incident {

struct Actor {
    bool combatant{};
    std::optional<std::uint32_t> classHash{};
    std::optional<std::uint64_t> player{};
};
struct Labels {
    std::array<std::uint32_t, 15> values{};
    std::size_t count{};
    std::span<const std::uint32_t> view() const noexcept {
        return std::span(values).first(count);
    }
};
struct Payload {
    std::uint32_t reorderSequence{};
    std::int32_t targetKind{};
    std::uint64_t recipient{};
    Actor killer{}, victim{};
    std::int32_t damage{};
    Labels source{}, actor{}, target{};
};

/** Mode-3 any_kill schema, raw-vector wire profile. No client memory or native layout input.
 * Unsupported profiles, truncation, extra data and nonzero padding are refused.
 * Decoding alone establishes neither an enemy death nor sender attribution.
 */
[[nodiscard]] bool decode_raw(std::span<const std::byte> bytes, Payload& output) noexcept;
} // namespace sunrise::middleware::bap::activity_message::kill_incident
