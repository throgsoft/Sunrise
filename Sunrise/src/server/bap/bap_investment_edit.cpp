#include <mutex>

#include "internal.h"
#include "investment_edit.h"
#include "state/account/inventory/inventory_edit.h"
#include "state/investment/investment_edit.h"

namespace sunrise::server::bap {

bool set_inventory_quantity(std::uint64_t character,
                            std::uint64_t instance,
                            std::uint32_t hash,
                            std::int32_t serial,
                            std::int32_t expected,
                            std::int32_t quantity) noexcept {
    const std::lock_guard lock(session_lock());
    if (!state::account::inventory::edit::set_quantity(
            character, instance, hash, serial, expected, quantity)) {
        return false;
    }
    arm_account_resync_everywhere();
    return true;
}

bool reset_season_pass_claim(std::uint16_t rewardIndex) noexcept {
    const std::lock_guard lock(session_lock());
    if (!state::reset_season_pass_reward(rewardIndex)) {
        return false;
    }
    arm_account_resync_everywhere();
    return true;
}

bool set_seasonal_experience(std::int32_t expected, std::int32_t replacement) noexcept {
    const std::lock_guard lock(session_lock());
    if (!state::replace_seasonal_experience(expected, replacement)) {
        return false;
    }
    for (auto& peer : sessions()) {
        peer.pendingSeasonalExperienceAmount = 0;
        peer.pendingSeasonalExperienceMutationSerial = 0;
    }
    arm_account_resync_everywhere();
    return true;
}

} // namespace sunrise::server::bap
