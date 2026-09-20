#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::build_data::season_pass {

void clear() noexcept;
[[nodiscard]] bool valid(std::span<const Reward> rewards) noexcept;
[[nodiscard]] bool replace(std::span<const Reward> rewards) noexcept;
[[nodiscard]] bool find(std::uint16_t rewardIndex, Reward& reward) noexcept;
[[nodiscard]] bool snapshot(std::span<Reward> output, std::size_t& count) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::season_pass
