#include "investment_edit.h"
#include "state/build_data/runtime.h"
#include "state/progression/season_pass_reward_catalog.h"
#include "state/runtime/runtime.h"
#include "state/unlocks/unlocks_runtime.h"
#include "store_internal.h"

namespace sunrise::state {

bool replace_seasonal_experience(std::int32_t expected, std::int32_t replacement) noexcept {
    investment::store::Transaction transaction;
    return replacement >= 0 && transaction.ready() && seasonal_experience() == expected
           && unlocks::set_account_progression(kArtifactPowerProgressionIndex, replacement)
           && seed_seasonal_progression() && transaction.commit();
}

bool reset_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    investment::store::Transaction transaction;
    build_data::season_pass::Reward reward{};
    if (!transaction.ready() || !build_data::find_season_pass_reward(rewardIndex, reward)
        || progression::season_pass::progress_flag(reward) != build_data::rewards::kAbsent
        || !season_pass_reward_claimed(rewardIndex)) {
        return false;
    }
    revoke_season_pass_reward(rewardIndex);
    return !season_pass_reward_claimed(rewardIndex) && transaction.commit();
}

} // namespace sunrise::state
