#include "season_pass_snapshot_runtime.h"

#include <algorithm>
#include <mutex>
#include <span>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/definition.h"
#include "runtime.h"

namespace sunrise::state {
namespace {

namespace pass = build_data::season_pass;

// An absent claim or acquisition flag lies past the account bank, so the bounds check skips it.
static_assert(unlocks::kAccountFlagCapacity <= pass::kUnavailableFlagIndex);
static_assert(unlocks::kAccountFlagCapacity <= build_data::rewards::kAbsent);

/** Saved values one pass evaluation reads, copied under one store lock. */
struct SavedPass {
    unlocks::Table banks{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    CharacterClass characterClass{};
    std::uint64_t revision{};
    std::int32_t experience{};
    std::uint16_t rank{};
};

/** Automatic rows and acquisition flags are read from the item and reward catalogs too. */
[[nodiscard]] bool catalogs_ready() noexcept {
    return build_data::season_pass_ready() && build_data::reward_definitions_ready()
           && build_data::item_definitions_ready();
}

/** Copies the revision, XP, the account banks and the selected character's banks together. */
[[nodiscard]] bool read_saved_pass(SavedPass& saved, const char*& reason) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    reason = "account_state";
    const AccountState account = investment::store::account();
    if (!account::valid(account) || account.primarySoid == 0) {
        return false;
    }
    saved.revision = investment::store::g_session.revision;
    saved.accountSoid = account.primarySoid;
    saved.experience = seasonal_experience();
    saved.rank = seasonal_rank();
    // The account banks are shared, so any slot reads them while no character is selected.
    int slot = 0;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            slot = static_cast<int>(index);
            saved.characterSoid = account.characters[index].soid;
            saved.characterClass = account.characters[index].characterClass;
            break;
        }
    }
    return investment::store::read_unlocks(saved.banks, slot);
}

/** @return True when the saved account flag is set; an absent index never is. */
[[nodiscard]] bool flag_set(const unlocks::Table& banks, std::uint16_t index) noexcept {
    return index < banks.accountFlags.size() && banks.accountFlags[index] == unlocks::kFlagSet;
}

/** Evaluates one native row against the saved copies; a placeholder row stays unavailable. */
[[nodiscard]] SeasonPassRowStatus row_status(std::uint16_t rewardIndex,
                                             const SavedPass& saved) noexcept {
    SeasonPassRowStatus row{};
    pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)) {
        return row;
    }
    row.itemHash = reward.itemHash;
    row.quantity = reward.quantity;
    row.requiredRank = reward.requiredRank;
    row.available = true;
    row.claimed = flag_set(saved.banks, reward.claimFlagIndex);
    row.automatic = progression::season_pass::progress_flag(reward) != build_data::rewards::kAbsent;
    build_data::rewards::Item source{};
    row.acquired = build_data::find_reward_item(reward.itemIndex, source)
                   && source.definitionHash == reward.itemHash
                   && flag_set(saved.banks, source.acquiredFlag);
    row.rankMet = reward.requiredRank <= saved.rank;
    // Conditions read a character's class and banks, so they wait for a selected character.
    if (saved.characterSoid != 0 && reward.conditionCount <= reward.condition.size()) {
        bool met = false;
        row.conditionKnown =
            reward_resolution::eligible(std::span(reward.condition).first(reward.conditionCount),
                                        {saved.banks, saved.characterClass, 0, nullptr},
                                        met);
        row.conditionMet = row.conditionKnown && met;
    }
    return row;
}

} // namespace

/** Reads one coherent saved pass view and evaluates every catalog row against it. */
bool read_season_pass_snapshot(SeasonPassSnapshot& output, const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    output = {};
    reason = build_data::reward_definitions_unsupported() ? "rewards_unsupported" : "not_ready";
    if (!catalogs_ready()) {
        return false;
    }
    SavedPass saved{};
    if (!read_saved_pass(saved, reason)) {
        return false;
    }
    output.accountSoid = saved.accountSoid;
    output.characterSoid = saved.characterSoid;
    output.investmentRevision = saved.revision;
    output.experience = saved.experience;
    output.rank = saved.rank;
    output.rowCount = (std::min)(build_data::season_pass_reward_count(), output.rows.size());
    for (std::size_t index = 0; index < output.rowCount; ++index) {
        output.rows[index] = row_status(static_cast<std::uint16_t>(index), saved);
    }
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
