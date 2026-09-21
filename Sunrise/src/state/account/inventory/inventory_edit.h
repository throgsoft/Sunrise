#pragma once

#include <cstdint>

namespace sunrise::state::account::inventory::edit {
/** Edits an unchanged, unequipped stack; zero removes the row without dismantle rewards. */
[[nodiscard]] bool set_quantity(std::uint64_t characterSoid,
                                std::uint64_t instanceSoid,
                                std::uint32_t definitionHash,
                                std::int32_t mutationSerial,
                                std::int32_t expectedQuantity,
                                std::int32_t quantity) noexcept;
} // namespace sunrise::state::account::inventory::edit
