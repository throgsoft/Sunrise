#pragma once

#include <cstdint>

namespace sunrise::server::studio {

/**
 * Answers one typed investment request per request id on the server thread, in the slice that
 * copies it. Studio creates the mapping; without it this only looks for one once a second.
 * @param now Monotonic tick count in milliseconds.
 */
void service_investment(std::uint64_t now) noexcept;

/** Unmaps the investment mapping; retained operation receipts are left to their owner. */
void shutdown_investment() noexcept;

} // namespace sunrise::server::studio
