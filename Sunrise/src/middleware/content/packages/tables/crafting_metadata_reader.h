#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../state/build_data/crafting/definition.h"

namespace sunrise::middleware::content::packages::tables::crafting {

/** Cross-checks the supported Chalice slot mappings before runtime bank access is enabled. */
[[nodiscard]] bool read_chalice_banks(std::span<const std::byte> flagSlots,
                                      std::span<const std::byte> valueSlots,
                                      std::span<const std::byte> flagMaps,
                                      std::span<const std::byte> valueMaps) noexcept;
/** Verifies lane identities and the absence of unhandled socket cost scalars. */
[[nodiscard]] bool read_chalice_sockets(std::span<const std::byte> bytes) noexcept;
/** Copies one plug's bounded predicates; output is usable only on success. */
[[nodiscard]] bool read_chalice_item(std::uint16_t index,
                                     std::uint32_t hash,
                                     std::span<const std::byte> bytes,
                                     state::build_data::crafting::ChalicePlug& output) noexcept;

/** Publishes costs only when all four supported socket types decode completely. */
[[nodiscard]] bool
read_synthesizer_costs(std::span<const std::byte> bytes,
                       state::build_data::crafting::SynthesizerCosts& output) noexcept;
/** Resolves the two emitted flags and refuses unhandled unlock side effects. */
[[nodiscard]] bool read_mote_output_flags(std::span<const std::byte> bytes,
                                          std::span<const std::byte> flagSlots,
                                          std::array<std::uint16_t, 2>& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables::crafting
