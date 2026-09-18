#pragma once
#include <cstdint>

#include "server_clock.h"

namespace sunrise::core::runtime {
/** Current server-issued investment time, anchored to the latest Family-5 stamp. */
[[nodiscard]] std::int64_t investment_clock_seconds() noexcept;
/** Preserves upstream's strictly increasing Family-5 stamps, including same-second pushes. */
[[nodiscard]] std::uint64_t next_family5_clock_seconds() noexcept;
/** Produces a positive signed item deadline, refusing invalid lifetimes or overflow. */
[[nodiscard]] bool investment_deadline(std::int64_t lifetimeSeconds,
                                       std::int32_t& deadline) noexcept;
} // namespace sunrise::core::runtime
