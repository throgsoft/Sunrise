#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "season_pass_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the season pass reward list is published. */
bool season_pass_ready() noexcept {
    return season_pass::count() != 0;
}

/** Publishes the season pass reward list and the wrapper items it grants, in one step. */
bool publish_season_pass(std::span<const season_pass::Reward> rewards,
                         std::span<const season_pass::Package> packages) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(season_pass::replace(rewards, packages), season_pass::clear);
}

/** Reads one season pass reward row. */
bool find_season_pass_reward(std::uint16_t rewardIndex, season_pass::Reward& reward) noexcept {
    return season_pass::find(rewardIndex, reward);
}

/** Finds the acquired item set, resolving the shipped pass's unfinished weapon alias. */
bool find_season_pass_package(std::uint32_t definitionHash,
                              season_pass::Package& package) noexcept {
    if (!season_pass::find_package(definitionHash, package)) {
        return false;
    }
    // The premium class gearsets name an unfinished Witherhoard definition with no power
    // metadata or combat plugs. The pass's standalone reward names the playable definition.
    // Resolve the identity before acquisition so its own native sockets and power rules apply;
    // keep the extracted catalog intact and use the same result for prepare and commit.
    constexpr std::uint32_t kUnfinishedWitherhoardHash = 2522817335U;
    constexpr std::uint32_t kWitherhoardHash = 2357297366U;
    for (std::size_t index = 0; index < package.itemCount; ++index) {
        if (package.items[index] != kUnfinishedWitherhoardHash) {
            continue;
        }
        package.items[index] = kWitherhoardHash;
    }
    return true;
}

/** @return Season pass reward rows in State. */
std::size_t season_pass_reward_count() noexcept {
    return season_pass::count();
}

} // namespace sunrise::state::build_data
