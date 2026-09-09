#pragma once

#include <cstdint>

namespace sunrise::client::material_toast {
/** Drains committed acquisitions on the Steam callback's game-thread client section. */
void pump(std::uint64_t now) noexcept;
/** Drops outstanding presentation when the client runtime shuts down. */
void shutdown() noexcept;
} // namespace sunrise::client::material_toast
