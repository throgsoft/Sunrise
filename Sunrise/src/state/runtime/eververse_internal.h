#pragma once

#include "eververse_runtime.h"

namespace sunrise::state::eververse::detail {
[[nodiscard]] bool wallet_balance(const AccountState& account,
                                  std::uint32_t currencyHash,
                                  std::int32_t& balance,
                                  std::int32_t& cap) noexcept;
[[nodiscard]] bool stage_wallet_balance(AccountState& account,
                                        std::uint32_t currencyHash,
                                        std::int64_t amount) noexcept;
} // namespace sunrise::state::eververse::detail
