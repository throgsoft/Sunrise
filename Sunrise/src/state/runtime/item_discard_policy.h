#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::item_discard {

/** How much of an owned stack one no-SOID opcode-402 request removes. */
enum class Mode : std::uint8_t { one, entireStack };

/** Gunsmith Materials is a wallet row the native page deletes whole rather than by unit. */
inline constexpr std::uint32_t kGunsmithMaterialsHash = 685'157'383U;

inline constexpr std::array<std::uint32_t, 1> kEntireStackHashes{{kGunsmithMaterialsHash}};

/** Every other installed stack deletes one unit per request, matching the character path. */
[[nodiscard]] constexpr Mode mode(std::uint32_t hash) noexcept {
    for (const auto entry : kEntireStackHashes)
        if (entry == hash) return Mode::entireStack;
    return Mode::one;
}

/** Observed quantity is a precondition; the mode chooses the whole row versus one unit. */
[[nodiscard]] constexpr std::int32_t
quantity(Mode selected, std::int32_t held, std::int32_t maxStack) noexcept {
    return held > 0 && held <= maxStack ? (selected == Mode::entireStack ? held : 1) : 0;
}

} // namespace sunrise::state::item_discard
