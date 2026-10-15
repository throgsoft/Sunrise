#pragma once

#include <cstddef>
#include <span>

#include "../internal.h"

namespace sunrise::server::bap::encrypted {

/**
 * Runs the oldest editor operation submitted against this peer's account and records its receipt.
 * Nothing commits unless its frame is copied out, except a grant whose frame failed once State
 * committed it, which every Family-4 peer then resyncs.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when the operation is published.
 */
[[nodiscard]] bool consume_investment_operation(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted
