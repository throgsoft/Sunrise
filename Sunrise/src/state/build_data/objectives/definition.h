#pragma once
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::objectives {
/** Bounded storage for the dense installed objective table. */
inline constexpr std::size_t kDefinitionCapacity = 12'288;
struct Definition {
    std::uint32_t definitionHash{};
    std::int32_t completionValue{};
    std::uint16_t definitionIndex{};
};
} // namespace sunrise::state::build_data::objectives
