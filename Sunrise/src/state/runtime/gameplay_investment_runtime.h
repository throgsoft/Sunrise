#pragma once
#include "../activity/definition.h"
#include "../bounties/gameplay_kill_investment.h"

namespace sunrise::state {
/** Server supplies the authenticated private owner and the message's reorder sequence.
 * The caller serializes the connection lease while State commits account and receipt together.
 */
[[nodiscard]] GameplayKillResult invest_gameplay_kill(const bounties::gameplay::Event& event,
                                                      const activity::SessionBinding& owner,
                                                      std::uint64_t playerKey,
                                                      std::uint32_t reorderSequence) noexcept;
} // namespace sunrise::state
