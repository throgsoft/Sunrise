#pragma once

#include "../account/account_state.h"
#include "../build_data/rewards/definition.h"
#include "../unlocks/definition.h"

namespace sunrise::state::rewards {

/** Native reward categories distinguish gear from accompanying currency or effects. */
inline constexpr std::uint32_t kGearCategory = 1172844112U;

struct Grant {
    std::uint16_t itemIndex{build_data::rewards::kAbsent};
    std::int32_t quantity{};
    std::array<build_data::rewards::SocketOverride, 12> sockets{};
    std::size_t socketCount{};
};

struct Result {
    std::array<Grant, build_data::rewards::kGrantCapacity> grants{};
    std::size_t count{};
};

/** The seed belongs to the prepared server transaction, so validation repeats the same draw. */
struct Context {
    const unlocks::Table& unlocks;
    CharacterClass characterClass{};
    std::uint64_t seed{};
};

[[nodiscard]] bool eligible(std::span<const build_data::rewards::Instruction> condition,
                            const Context& context,
                            bool& result) noexcept;

/** A zero category expands every declared selector; a named category expands only that lane. */
[[nodiscard]] bool resolve(build_data::rewards::View definitions,
                           const Context& context,
                           std::uint16_t itemIndex,
                           std::uint32_t quantity,
                           std::uint32_t category,
                           Result& result) noexcept;
[[nodiscard]] bool resolve(const Context& context,
                           std::uint16_t itemIndex,
                           std::uint32_t quantity,
                           std::uint32_t category,
                           Result& result) noexcept;

} // namespace sunrise::state::rewards
