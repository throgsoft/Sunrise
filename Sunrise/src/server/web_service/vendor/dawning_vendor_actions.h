#pragma once
#include <cstdint>

namespace sunrise::server::web_service {
struct Outcome;
}

namespace sunrise::server::web_service::vendor {
/** Call after upstream row/interaction resolution, before a generic grant or substitution.
 * True owns the request; a successful exchange prepares the existing record-reward mutation. */
[[nodiscard]] bool intercept_dawning_delivery(std::uint16_t opcode,
                                              std::int32_t vendorIndex,
                                              std::int32_t saleIndex,
                                              std::uint16_t itemDefinitionIndex,
                                              Outcome& outcome) noexcept;
} // namespace sunrise::server::web_service::vendor
