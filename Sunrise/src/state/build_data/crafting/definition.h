#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::crafting {

/** Supported Chalice identity and the item-index span containing its socket plugs. */
inline constexpr std::uint32_t kChaliceHash = 1115550924U;
inline constexpr std::uint16_t kChaliceIndex = 7937;
inline constexpr std::uint16_t kChaliceLastIndex = 8019;
inline constexpr std::size_t kChalicePlugCapacity = kChaliceLastIndex - kChaliceIndex + 1;
/** Twelve rune balances in account objectives; thirteen upgrades in account acquired flags. */
inline constexpr std::uint16_t kRuneValueBase = 2371;
inline constexpr std::uint16_t kUpgradeFlagBase = 5427;
/** Account objective row for the Chalice introduction quest. */
inline constexpr std::uint16_t kQuestValue = 5662;
/** Unlock-slot identities corresponding to the contiguous upgrade bank above. */
inline constexpr std::array<std::uint16_t, 13> kChaliceFlagSlots{
    8698, 8699, 8700, 8703, 8704, 8705, 8706, 8707, 8708, 8709, 8710, 8711, 8712};
/** Socket-type indices in lane order; the package reader cross-checks their definition hashes. */
inline constexpr std::array<std::uint16_t, 8> kChaliceSocketTypes{
    615, 611, 612, 613, 619, 620, 621, 614};

/** Container/empty plugs, three sets of twelve runes, then upgrades and their result plugs. */
[[nodiscard]] constexpr bool needs_chalice_item(std::uint16_t index) noexcept {
    return (index >= kChaliceIndex && index <= 7940) || (index >= 7949 && index <= 7984)
           || (index >= 7997 && index <= kChaliceLastIndex);
}

/** Bounded investment predicates, decoded from package expressions without retaining bytes. */
struct Instruction {
    std::uint32_t op{}, operand{};
};
struct Expression {
    std::array<Instruction, 8> code{};
    std::size_t count{};
};
struct ChalicePlug {
    std::uint32_t hash{};
    std::array<Expression, 3> rules{};
    std::size_t ruleCount{};
    bool ready{};
};

/** Item-specific multiplier applied to an installed socket's material requirements. */
struct Scalar {
    std::uint16_t itemIndex{};
    std::uint32_t multiplier{};
};
struct SocketCost {
    std::uint16_t socketType{};
    std::size_t count{};
    std::array<Scalar, 4> scalars{};
};
using SynthesizerCosts = std::array<SocketCost, 4>;

/** A real Mote emits a shared tier flag and a distinct role/tier ownership flag. */
struct MoteOutput {
    std::uint32_t hash{};
    std::array<std::uint16_t, 2> slots{};
};

} // namespace sunrise::state::build_data::crafting
