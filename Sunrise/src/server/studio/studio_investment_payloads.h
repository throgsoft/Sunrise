#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/studio/studio_investment_wire.h"
#include "../bap/investment_operations.h"

namespace sunrise::server::studio {

/** The response payload area every investment encoder writes into. */
using InvestmentPayload =
    std::span<std::byte, middleware::studio::investment_wire::kResponsePayloadBytes>;

/** Everything one investment answer carries besides its payload bytes. */
struct InvestmentAnswer {
    middleware::studio::investment_wire::Status status{};
    middleware::studio::investment_wire::Context context{};
    /** Static ASCII label: why the request was refused, or the label its receipt carries. */
    const char* reason{""};
    /** The opcode's fixed payload size once the payload is written, else zero. */
    std::uint32_t payloadBytes{};
};

/**
 * Encodes the live account, catalog readiness and publication peer an editor binds to.
 * @param gameStarted The game process instance this link advertises.
 * @param capabilities The opcodes this link serves.
 */
[[nodiscard]] InvestmentAnswer encode_hello(std::uint64_t gameStarted,
                                            std::uint64_t capabilities,
                                            InvestmentPayload payload) noexcept;

/** Encodes the profile and the selected character's items from one saved account copy. */
[[nodiscard]] InvestmentAnswer encode_inventory_snapshot(InvestmentPayload payload) noexcept;

/** Encodes one coherent Season pass view, every catalog row evaluated without a draw. */
[[nodiscard]] InvestmentAnswer encode_season_pass_snapshot(InvestmentPayload payload) noexcept;

/** Encodes the selected character's held pursuits and their saved completion status. */
[[nodiscard]] InvestmentAnswer encode_pursuit_snapshot(InvestmentPayload payload) noexcept;

/**
 * Encodes a receipt bound to its operation's target; an empty receipt answers unknown.
 * @param operationId The id the editor asked about, echoed even when no receipt is retained.
 */
[[nodiscard]] InvestmentAnswer encode_receipt(std::uint64_t operationId,
                                              const bap::InvestmentReceipt& receipt,
                                              InvestmentPayload payload) noexcept;

} // namespace sunrise::server::studio
