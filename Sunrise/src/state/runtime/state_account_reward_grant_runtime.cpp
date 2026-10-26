/**
 * Grants that no purchase pays for: season pass rewards, record rewards, and the
 * default emote collection.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/unlocks_records.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

namespace {

/** Prepares inventory rows and consumed-wrapper flags against the same account view. */
[[nodiscard]] RewardPreparation
prepare_reward_grant(std::span<const DirectRecordReward> rewards,
                     std::uint16_t claimedRecordIndex,
                     const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags,
                     PendingRecordRewardGrant& mutation,
                     const char** refusal) noexcept;

[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept;

/** @return The kind of item a reward grants; overflow can still land a stack in its recovery
 * bucket. */
[[nodiscard]] RecordRewardKind reward_kind(const item_details::Definition& detail,
                                           const inventory_buckets::Descriptor& bucket) noexcept {
    if (detail.instancedDefinitionState == item_details::InstancedDefinitionState::instanced) {
        return RecordRewardKind::characterInstance;
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::profile
               ? RecordRewardKind::profileStack
               : RecordRewardKind::characterStack;
}

/** A reward overrides socket types, so adding an ordinary lane cannot shift a fixed roll. */
[[nodiscard]] bool
apply_reward_sockets(const item_details::Definition& detail,
                     std::span<const build_data::rewards::SocketOverride> overrides,
                     authored_inventory::Sockets& sockets,
                     const char** refusal = nullptr) noexcept {
    const auto fail = [&](const char* reason) noexcept {
        if (refusal != nullptr) {
            *refusal = reason;
        }
        return false;
    };
    if (overrides.empty()) {
        return true;
    }
    if (detail.ordinarySocketCount > sockets.plugs.size()) {
        return fail("socket_layout");
    }
    authored_inventory::Sockets staged{};
    staged.policy = authored_inventory::SocketPolicy::authored;
    staged.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < staged.plugCount; ++lane) {
        const auto index = detail.initialPlugIndices[lane];
        if (index == item_details::kUnavailableItemIndex) {
            continue;
        }
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_index(index, plug)) {
            return fail("socket_layout");
        }
        staged.plugs[lane] = plug.definitionHash;
    }
    std::array<bool, item_details::kInitialPlugCapacity> replaced{};
    for (const auto& override : overrides) {
        if (override.plugSet != build_data::rewards::kAbsent) {
            return fail("socket_plug_set");
        }
        if (override.rollSet != build_data::rewards::kAbsent) {
            return fail("socket_roll_set");
        }
        // Zero selection preserves the initial plug in every lane of the named type.
        const bool preserve =
            override.plugItem == build_data::rewards::kAbsent && override.selection == 0;
        if (!preserve && override.selection != build_data::rewards::kFixedPlugSelection) {
            return fail("socket_selection");
        }
        build_data::items::Definition plug{};
        if (!preserve && !build_data::find_item_definition_index(override.plugItem, plug)) {
            return fail("socket_layout");
        }
        bool found = false;
        for (std::size_t lane = 0; lane < staged.plugCount; ++lane) {
            if (detail.socketTypes[lane] != override.socketType) {
                continue;
            }
            if (replaced[lane] || (!preserve && found)) {
                return fail("socket_layout");
            }
            replaced[lane] = true;
            found = true;
            if (!preserve) {
                staged.plugs[lane] = plug.definitionHash;
            }
        }
        if (!found) {
            return fail("socket_layout");
        }
    }
    if (!authored_inventory::valid(staged)) {
        return fail("socket_layout");
    }
    sockets = staged;
    return true;
}

[[nodiscard]] RewardPreparation prepare_resolved_reward(const reward_resolution::Result& resolved,
                                                        PendingRecordRewardGrant& mutation,
                                                        const char** refusal) noexcept {
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rows{};
    if (resolved.count > rows.size()) {
        return RewardPreparation::unresolvable;
    }
    for (std::size_t i = 0; i < resolved.count; ++i) {
        const auto& grant = resolved.grants[i];
        rows[i] = {grant.itemIndex,
                   grant.quantity,
                   std::span(grant.sockets).first(grant.socketCount),
                   true};
    }
    return prepare_reward_grant(std::span(rows).first(resolved.count),
                                kUnclaimedRecordIndex,
                                &resolved.wrapperFlags,
                                mutation,
                                refusal);
}

/** Classifies a catalog refusal: a verdict unless a catalog was cleared meanwhile. */
[[nodiscard]] RewardPreparation catalog_refusal(const char*& reason) noexcept {
    if (build_data::item_definitions_ready() && build_data::reward_definitions_ready()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    return RewardPreparation::deferred;
}

/**
 * True for wrappers and direct account perks. Both grant through their reward definition and
 * bypass quest initialization.
 */
bool uses_reward_definition(const build_data::items::Definition& item) noexcept {
    build_data::rewards::Item reward{};
    if (!build_data::find_reward_item(item.definitionIndex, reward)) {
        return false;
    }
    return reward.poolIndex != build_data::rewards::kAbsent
           || (item.bucketId == inventory_buckets::kPerkBucketId
               && reward.acquiredFlag != build_data::rewards::kAbsent);
}

enum class PassResolution { claim, replay };

[[nodiscard]] bool resolve_pass(const build_data::season_pass::Reward& reward,
                                const AccountState& account,
                                std::uint64_t seed,
                                reward_resolution::Result& result,
                                PassResolution resolution,
                                const char** reason = nullptr) noexcept {
    const auto character = selected_character_index(account);
    unlocks::Table flags{};
    build_data::rewards::Item source{};
    if (character >= account.characterCount
        || !investment::store::read_unlocks(flags, static_cast<int>(character))
        || !build_data::find_reward_item(reward.itemIndex, source)
        || source.definitionHash != reward.itemHash || reward.socketCount > reward.sockets.size()
        || reward.conditionCount > reward.condition.size()) {
        return false;
    }
    if (reason != nullptr) {
        *reason = "reward_condition";
    }
    if (resolution == PassResolution::replay && reward.claimFlagIndex < flags.accountFlags.size()) {
        flags.accountFlags[reward.claimFlagIndex] = unlocks::kFlagClear;
    }
    bool enabled = false;
    if (!reward_resolution::eligible(
            std::span(reward.condition).first(reward.conditionCount),
            {flags, account.characters[character].characterClass, seed, reason},
            enabled)
        || !enabled) {
        return false;
    }
    if (reward_resolution::resolve(
            {flags, account.characters[character].characterClass, seed, reason},
            reward.itemIndex,
            reward.quantity,
            result)
        != reward_resolution::Resolution::resolved) {
        return false;
    }
    if (reward.socketCount != 0) {
        if (result.count != 1 || result.grants[0].itemIndex != reward.itemIndex) {
            if (reason != nullptr) {
                *reason = "socket_owner";
            }
            return false;
        }
        result.grants[0].sockets = reward.sockets;
        result.grants[0].socketCount = reward.socketCount;
    }
    return true;
}

[[nodiscard]] bool reward_matches(const build_data::season_pass::Reward& reward,
                                  const PendingSeasonPassReward& mutation) noexcept {
    const auto* grant = &mutation.grant;
    reward_resolution::Result expected{};
    if (!mutation.prepared || mutation.sourceDefinitionHash != reward.itemHash
        || !resolve_pass(
            reward, account_snapshot(), mutation.seed, expected, PassResolution::replay)
        || expected.count != grant->rewardCount
        || expected.wrapperFlags.count() != grant->wrapperFlags.size()) {
        return false;
    }
    for (const auto& flag : grant->wrapperFlags) {
        if (flag.index >= expected.wrapperFlags.size() || !expected.wrapperFlags[flag.index]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < expected.count; ++i) {
        const auto& planned = expected.grants[i];
        const auto& prepared = grant->rewards[i];
        build_data::items::Definition definition{};
        build_data::rewards::Item source{};
        if (!build_data::find_item_definition_index(planned.itemIndex, definition)
            || !build_data::find_reward_item(planned.itemIndex, source)
            || prepared.definitionHash != definition.definitionHash
            || prepared.quantity != planned.quantity
            || prepared.acquiredFlag != source.acquiredFlag) {
            return false;
        }
        // A dropped or later discarded instance leaves no sockets to compare.
        const auto held = std::span(grant->afterCharacter.inventory.values)
                              .first(grant->afterCharacter.inventory.count);
        const auto installed =
            std::find_if(held.begin(), held.end(), [&](const auto& item) noexcept {
                return prepared.instanceSoid != 0 && item.instanceSoid == prepared.instanceSoid;
            });
        if (planned.socketCount != 0 && installed != held.end()) {
            item_details::Definition detail{};
            authored_inventory::Sockets sockets{};
            if (prepared.kind != RecordRewardKind::characterInstance
                || !build_data::find_configured_item_detail(planned.itemIndex, detail)
                || !apply_reward_sockets(
                    detail, std::span(planned.sockets).first(planned.socketCount), sockets)
                || installed->sockets.policy != sockets.policy
                || installed->sockets.plugCount != sockets.plugCount
                || installed->sockets.plugs != sockets.plugs) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool prepare_season_pass_reward(std::uint16_t rewardIndex,
                                std::uint64_t seed,
                                PendingSeasonPassReward& mutation,
                                const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_index";
    mutation = {};
    const std::lock_guard lock(investment::store::g_mutex);
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)) {
        return false;
    }
    reason = "rank";
    if (reward.requiredRank > seasonal_rank()) {
        return false;
    }
    reason = "already_claimed";
    if (season_pass_reward_claimed(rewardIndex)) {
        return false;
    }
    mutation.seed = seed;
    reward_resolution::Result resolved{};
    reason = "reward_condition";
    if (!resolve_pass(
            reward, account_snapshot(), mutation.seed, resolved, PassResolution::claim, &reason)) {
        return false;
    }
    reason = "reward_placement";
    if (prepare_resolved_reward(resolved, mutation.grant, &reason) != RewardPreparation::prepared) {
        return false;
    }
    reason = nullptr;
    mutation.sourceDefinitionHash = reward.itemHash;
    mutation.rewardIndex = rewardIndex;
    mutation.prepared = true;
    return true;
}

ItemGrantRoute item_grant_route(std::uint16_t itemIndex) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return ItemGrantRoute::unavailable;
    }
    if (uses_reward_definition(item)) {
        return ItemGrantRoute::reward;
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_configured_item_detail(itemIndex, detail)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return ItemGrantRoute::unavailable;
    }
    const bool profileStack =
        bucket.arraySelector == inventory_buckets::ArraySelector::profile
        && detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable;
    return profileStack || bucket.arraySelector == inventory_buckets::ArraySelector::character
               ? ItemGrantRoute::reward
               : ItemGrantRoute::unavailable;
}

std::int32_t item_grant_step(std::uint16_t itemIndex, std::int32_t remaining) noexcept {
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (remaining <= 1 || !build_data::find_item_definition_index(itemIndex, item)
        || uses_reward_definition(item)
        || !build_data::find_configured_item_detail(itemIndex, detail)
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0) {
        return (std::min)(remaining, 1);
    }
    return (std::min)(remaining, detail.maxStackSize);
}

std::int32_t item_grant_capacity(std::uint16_t itemIndex) noexcept {
    constexpr std::int32_t kLargestGrant = (std::numeric_limits<std::int32_t>::max)();
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return 0;
    }
    // A wrapper draws on every step, and each draw lands wherever its pool sends it.
    build_data::rewards::Item reward{};
    if (build_data::find_reward_item(itemIndex, reward)
        && reward.poolIndex != build_data::rewards::kAbsent) {
        return kLargestGrant;
    }
    // An account perk is one acquisition flag.
    if (uses_reward_definition(item)) {
        return 1;
    }
    inventory_buckets::Descriptor own{};
    if (!build_data::find_inventory_bucket_descriptor(item.bucketId, own)) {
        return 0;
    }
    std::int64_t rows = own.slotCount;
    inventory_buckets::Descriptor recovery{};
    if (find_recovery_bucket(item, recovery) && recovery.bucketId != own.bucketId) {
        rows += recovery.slotCount;
    }
    const std::int64_t capacity = rows * item_grant_step(itemIndex, kLargestGrant);
    return static_cast<std::int32_t>((std::min)(capacity, std::int64_t{kLargestGrant}));
}

RewardPreparation prepare_item_reward(std::uint16_t itemIndex,
                                      std::uint32_t quantity,
                                      std::uint64_t seed,
                                      PendingRecordRewardGrant& mutation,
                                      const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    mutation = {};
    // Unpublished domains decide nothing; retry later.
    if (!build_data::item_definitions_ready() || !build_data::reward_definitions_ready()) {
        return RewardPreparation::deferred;
    }
    reason = "item_definition";
    const std::lock_guard lock(investment::store::g_mutex);
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return catalog_refusal(reason);
    }
    reason = "selected_character";
    // A whole account is large, so its snapshot is owned on the heap like the claim path's.
    const std::unique_ptr<const AccountState> snapshot(new (std::nothrow)
                                                           AccountState(account_snapshot()));
    if (!snapshot) {
        return RewardPreparation::deferred;
    }
    const AccountState& account = *snapshot;
    const auto character = selected_character_index(account);
    unlocks::Table flags{};
    if (character >= account.characterCount
        || !investment::store::read_unlocks(flags, static_cast<int>(character))) {
        return RewardPreparation::deferred;
    }
    reward_resolution::Result resolved{};
    const auto resolution = reward_resolution::resolve(
        {flags, account.characters[character].characterClass, seed, &reason},
        itemIndex,
        quantity,
        resolved);
    // Unlocks can change and another draw can fit, so these are retried, not retired.
    if (resolution == reward_resolution::Resolution::ineligible
        || resolution == reward_resolution::Resolution::exceeded) {
        return RewardPreparation::deferred;
    }
    if (resolution != reward_resolution::Resolution::resolved) {
        return catalog_refusal(reason);
    }
    reason = "reward_placement";
    return prepare_resolved_reward(resolved, mutation, &reason);
}

bool preview_reward_unlocks(const PendingRecordRewardGrant& mutation,
                            unlocks::Table& after) noexcept {
    if (!mutation.prepared || mutation.rewardCount > mutation.rewards.size()
        || !investment::store::read_unlocks(after, static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        if (reward.acquiredFlag == build_data::rewards::kAbsent) {
            continue;
        }
        if (reward.acquiredFlag >= after.accountFlags.size()
            || after.accountFlags[reward.acquiredFlag] != reward.previousFlag) {
            return false;
        }
    }
    for (std::size_t i = 0; i < mutation.wrapperFlags.size(); ++i) {
        const auto& flag = mutation.wrapperFlags[i];
        if (flag.index >= after.accountFlags.size() || flag.previousValue > unlocks::kFlagSet
            || (i != 0 && mutation.wrapperFlags[i - 1].index >= flag.index)
            || after.accountFlags[flag.index] != flag.previousValue) {
            return false;
        }
    }
    for (const auto& flag : mutation.wrapperFlags) {
        after.accountFlags[flag.index] = unlocks::kFlagSet;
    }
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto flag = mutation.rewards[i].acquiredFlag;
        if (flag != build_data::rewards::kAbsent) {
            after.accountFlags[flag] = unlocks::kFlagSet;
        }
    }
    // A placed quest item sets its first step only while the saved value it observed still holds.
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        const auto& quest = reward.questInitialization;
        if (quest.scope == build_data::items::QuestInitialization::Scope::none) {
            continue;
        }
        const std::span<std::int32_t> values =
            quest.scope == build_data::items::QuestInitialization::Scope::account
                ? std::span<std::int32_t>(after.objectiveValues)
                : std::span<std::int32_t>(after.characterObjectValues);
        if (!build_data::items::valid(quest) || quest.row >= values.size()
            || values[quest.row] != reward.previousQuestValue) {
            return false;
        }
        values[quest.row] = build_data::items::initialized_value(quest, reward.previousQuestValue);
    }
    return true;
}

/** Revalidates the prepared reward under the lock that commits its inventory and flags. */
bool commit_season_pass_reward(PendingSeasonPassReward& mutation) noexcept {
    if (!mutation.prepared) {
        return false;
    }
    const PendingConsumption consume{mutation};
    bool ready = false;
    {
        investment::store::Transaction transaction;
        build_data::season_pass::Reward reward{};
        ready = transaction.ready()
                && build_data::find_season_pass_reward(mutation.rewardIndex, reward)
                && season_pass_reward_claimed(mutation.rewardIndex)
                && reward.requiredRank <= seasonal_rank() && reward_matches(reward, mutation)
                && commit_record_reward(mutation.grant) && transaction.commit();
    }
    if (!ready) {
        revoke_season_pass_reward(mutation.rewardIndex);
    }
    return ready;
}

namespace {

/**
 * Rebuilds the account after-image for one prepared reward set and rejects any drift.
 * @param current Live account the mutation was prepared against.
 * @param after Receives the after-image; its content is unusable on failure.
 * @return False when the account moved or a reward row breaks its own item rules.
 */
[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept {
    if (!mutation.prepared
        || (mutation.rewardCount == 0 && mutation.wrapperFlags.empty() && mutation.rowCount == 0)
        || mutation.rewardCount > mutation.rewards.size()
        || mutation.rowCount > mutation.rows.size()
        || change_ring_refusal(std::span(mutation.rows).first(mutation.rowCount)) != nullptr
        || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.characterIndex >= current.characterCount
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.beforeProfileItemCount)
        || !mutation.beforeCharacter.selected
        || mutation.beforeCharacter.soid != mutation.characterSoid) {
        return false;
    }

    after = current;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(after) || !valid_profile_inventory(after)
        || !family4_loadout::resolve(after, mutation.characterIndex, loadout)) {
        return false;
    }

    for (std::size_t index = 0; index < mutation.rewardCount; ++index) {
        const PreparedRecordReward& reward = mutation.rewards[index];
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (reward.definitionHash == authored_inventory::kNoDefinitionHash || reward.quantity <= 0
            || !build_data::find_item_definition_hash(reward.definitionHash, item)) {
            return false;
        }
        if (reward.kind == RecordRewardKind::accountUnlock) {
            build_data::rewards::Item source{};
            if (reward.quantity != 1 || reward.instanceSoid != 0
                || item.bucketId != build_data::inventory::buckets::kPerkBucketId
                || !build_data::find_reward_item(item.definitionIndex, source)
                || source.acquiredFlag == build_data::rewards::kAbsent
                || source.acquiredFlag != reward.acquiredFlag) {
                return false;
            }
            continue;
        }
        if (!build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || reward.kind != reward_kind(detail, bucket)
            || (reward.kind != RecordRewardKind::characterInstance && reward.instanceSoid != 0)
            || (reward.kind == RecordRewardKind::characterInstance && reward.quantity != 1)) {
            return false;
        }
    }
    // Every announced row is in the after-image exactly once.
    for (std::size_t index = 0; index < mutation.rowCount; ++index) {
        const GrantedRow& row = mutation.rows[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mutation.rows[prior].array == row.array
                && mutation.rows[prior].mutationSerial == row.mutationSerial) {
                return false;
            }
        }
        if (!row_present(after, mutation.characterIndex, row)) {
            return false;
        }
    }
    return true;
}

/** Captures only flags carried by consumed wrappers; storage failure remains retryable. */
[[nodiscard]] bool prepare_wrapper_flags(const std::bitset<unlocks::kAccountFlagCapacity>* flags,
                                         PendingRecordRewardGrant& mutation,
                                         const char*& reason) noexcept {
    if (flags == nullptr) {
        return true;
    }
    try {
        mutation.wrapperFlags.reserve(flags->count());
        for (std::size_t index = 0; index < flags->size(); ++index) {
            if (!(*flags)[index]) {
                continue;
            }
            reason = "wrapper_acquisition_flag";
            std::int32_t before = 0;
            if (!investment::store::read_unlock(investment::store::Bank::accountFlags,
                                                static_cast<std::uint16_t>(index),
                                                before)
                || before < 0 || before > unlocks::kFlagSet) {
                return false;
            }
            mutation.wrapperFlags.push_back(
                {static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(before)});
        }
    } catch (...) {
        reason = "wrapper_flag_storage";
        return false;
    }
    return true;
}

/** Prepares inventory rows and consumed-wrapper flags against the same account view. */
RewardPreparation
prepare_reward_grant(std::span<const DirectRecordReward> rewards,
                     std::uint16_t claimedRecordIndex,
                     const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags,
                     PendingRecordRewardGrant& mutation,
                     const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_count";
    mutation = {};
    if ((rewards.empty() && (wrapperFlags == nullptr || wrapperFlags->none()))
        || rewards.size() > mutation.rewards.size()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    // The loadout check needs these domains, so nothing is decided before they publish.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::inventory_bucket_descriptors_ready()
        || !build_data::socket_entry_lists_ready()) {
        return RewardPreparation::deferred;
    }
    reason = "account_state";
    // Both whole-account images are owned on the heap like the claim path's.
    const std::unique_ptr<const AccountState> snapshot(new (std::nothrow)
                                                           AccountState(account_snapshot()));
    const std::unique_ptr<AccountState> image(new (std::nothrow) AccountState{});
    if (!snapshot || !image) {
        return RewardPreparation::deferred;
    }
    const AccountState& account = *snapshot;
    const std::size_t characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount) {
        return RewardPreparation::deferred;
    }

    if (!prepare_wrapper_flags(wrapperFlags, mutation, reason)) {
        return RewardPreparation::deferred;
    }
    AccountState& working = *image;
    working = account;
    // Identities stay above the pre-eviction inventory, so a discarded resident is never reused.
    std::uint64_t nextInstanceSoid = 0;
    PlacedRows placed;
    reason = "instance_capacity";
    if (!next_item_instance_soid(account, nextInstanceSoid)) {
        return RewardPreparation::deferred;
    }
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        reason = "item_identity";
        const DirectRecordReward& requested = rewards[index];
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requested.quantity <= 0
            || !build_data::find_item_definition_index(requested.itemDefinitionIndex, item)) {
            return RewardPreparation::unresolvable;
        }
        PreparedRecordReward prepared{};
        prepared.definitionHash = item.definitionHash;
        prepared.quantity = requested.quantity;
        if (requested.acquireUnlock) {
            reason = "acquisition_flag";
            // Only a resolved reward asks for its flag, so the graph is published.
            build_data::rewards::Item source{};
            if (!build_data::find_reward_item(item.definitionIndex, source)
                || source.definitionHash != item.definitionHash) {
                return RewardPreparation::unresolvable;
            }
            prepared.acquiredFlag = source.acquiredFlag;
            if (source.acquiredFlag != build_data::rewards::kAbsent) {
                std::int32_t before = 0;
                // The saved flag refuses here, not the definition.
                if (!investment::store::read_unlock(
                        investment::store::Bank::accountFlags, source.acquiredFlag, before)
                    || before < 0 || before > unlocks::kFlagSet) {
                    return RewardPreparation::deferred;
                }
                prepared.previousFlag = static_cast<std::uint8_t>(before);
            }
        }
        if (item.bucketId == build_data::inventory::buckets::kPerkBucketId) {
            reason = "perk_acquisition";
            if (prepared.acquiredFlag == build_data::rewards::kAbsent || requested.quantity != 1
                || !requested.sockets.empty()) {
                return RewardPreparation::unresolvable;
            }
            prepared.kind = RecordRewardKind::accountUnlock;
            prepared.placedQuantity = 1;
            mutation.rewards[index] = prepared;
            continue;
        }
        reason = "item_detail";
        if (!build_data::find_configured_item_detail(requested.itemDefinitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
            return RewardPreparation::unresolvable;
        }
        reason = "inventory_destination";
        prepared.kind = reward_kind(detail, bucket);
        const bool instance = prepared.kind == RecordRewardKind::characterInstance;
        if ((bucket.arraySelector != inventory_buckets::ArraySelector::character
             && bucket.arraySelector != inventory_buckets::ArraySelector::profile)
            || (instance
                && (bucket.arraySelector != inventory_buckets::ArraySelector::character
                    || requested.quantity != 1))
            || (!instance && (detail.equipmentSlot.has_value() || !requested.sockets.empty()))) {
            return RewardPreparation::unresolvable;
        }
        Arrival arrival{item, detail, requested.quantity};
        if (instance) {
            arrival.instance.level = acquisition_level(working.characters[characterIndex]);
            arrival.instance.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
            if (!apply_reward_sockets(
                    detail, requested.sockets, arrival.instance.sockets, &reason)) {
                return RewardPreparation::unresolvable;
            }
        }
        ArrivalOutcome outcome{};
        if (!place_arrival(
                working, characterIndex, arrival, nextInstanceSoid, placed, outcome, reason)) {
            return RewardPreparation::deferred;
        }
        prepared.placedQuantity = outcome.placed;
        if (instance && outcome.placed != 0) {
            prepared.instanceSoid = nextInstanceSoid - 1;
        }
        // Only an item that found a row runs its acquisition side effects.
        if (outcome.placed != 0
            && item.questInitialization.scope
                   != build_data::items::QuestInitialization::Scope::none) {
            reason = "quest_state";
            prepared.questInitialization = item.questInitialization;
            if (!build_data::items::valid(item.questInitialization)
                || !investment::store::read_unlock(quest_bank(item.questInitialization),
                                                   item.questInitialization.row,
                                                   prepared.previousQuestValue)) {
                return RewardPreparation::deferred;
            }
        }
        mutation.rewards[index] = prepared;
    }

    reason = "loadout";
    // A full native bucket rejects the whole loadout; retry later.
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)) {
        return RewardPreparation::deferred;
    }
    record_grant_images(account, working, characterIndex, placed, mutation);
    mutation.claimedRecordIndex = claimedRecordIndex;
    mutation.rewardCount = rewards.size();
    return RewardPreparation::prepared;
}

} // namespace

/** A reward with no acquisition flag leaves its collectible state unknown, so it marks nothing. */
bool newly_collected(const PendingRecordRewardGrant& mutation,
                     const GrantedRow& row,
                     const build_data::items::Definition& item) noexcept {
    if (!row.newRow || mutation.rewardCount > mutation.rewards.size()
        || !build_data::collectibles::grants_item(item.definitionIndex)) {
        return false;
    }
    const auto rewards = std::span(mutation.rewards).first(mutation.rewardCount);
    return std::any_of(rewards.begin(), rewards.end(), [&](const PreparedRecordReward& reward) {
        return reward.definitionHash == item.definitionHash
               && reward.acquiredFlag != build_data::rewards::kAbsent
               && reward.previousFlag != unlocks::kFlagSet;
    });
}

/** Diffs the grant's images by instance identity, new character instances first. */
bool granted_residents(const PendingRecordRewardGrant& mutation,
                       GrantedResidents& output) noexcept {
    output = {};
    const auto append = [&](std::uint64_t soid) noexcept {
        if (output.count == output.soids.size()) {
            return false;
        }
        output.soids[output.count++] = soid;
        return true;
    };
    const auto& before = mutation.beforeCharacter.inventory;
    const auto& after = mutation.afterCharacter.inventory;
    for (std::size_t index = 0; index < after.count; ++index) {
        const std::uint64_t soid = after.values[index].instanceSoid;
        const auto held = std::span(before.values).first(before.count);
        if (std::none_of(held.begin(),
                         held.end(),
                         [&](const auto& item) noexcept { return item.instanceSoid == soid; })
            && !append(soid)) {
            return false;
        }
    }
    const auto priorProfile =
        std::span(mutation.beforeProfileItems).first(mutation.beforeProfileItemCount);
    for (std::size_t index = 0; index < mutation.afterProfileItemCount; ++index) {
        const std::uint64_t soid = mutation.afterProfileItems[index].instanceSoid;
        if (soid != 0
            && std::none_of(priorProfile.begin(),
                            priorProfile.end(),
                            [&](const auto& item) noexcept { return item.instanceSoid == soid; })
            && !append(soid)) {
            return false;
        }
    }
    return true;
}

/** Prepares every direct reward over one cumulative account view. */
RewardPreparation prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                              std::uint16_t claimedRecordIndex,
                                              PendingRecordRewardGrant& mutation,
                                              const char** refusal) noexcept {
    return prepare_reward_grant(rewards, claimedRecordIndex, nullptr, mutation, refusal);
}

bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    after = {};
    return materialize_record_reward(account_snapshot(), mutation, after);
}

namespace {

/** Logs each reward the client's overflow rules discarded, in whole or in part. */
void report_dropped_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        if (reward.placedQuantity < reward.quantity) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=reward_grant stage=commit result=dropped item=%u requested=%d "
                              "placed=%d",
                              static_cast<unsigned>(reward.definitionHash),
                              static_cast<int>(reward.quantity),
                              static_cast<int>(reward.placedQuantity));
        }
    }
}

} // namespace

/** Commits the shared reward after-image and claim together. */
bool commit_record_reward(PendingRecordRewardGrant& mutation) noexcept {
    if (!mutation.prepared) {
        return false;
    }
    const PendingConsumption consume{mutation};
    bool ready = false;
    {
        investment::store::Transaction transaction;
        AccountState after{};
        unlocks::Table flags{};
        ready = transaction.ready()
                && materialize_record_reward(investment::store::account(), mutation, after)
                && preview_reward_unlocks(mutation, flags)
                && investment::store::write_account(after);
        for (std::size_t i = 0; ready && i < mutation.rewardCount; ++i) {
            const auto flag = mutation.rewards[i].acquiredFlag;
            if (flag != build_data::rewards::kAbsent) {
                ready = investment::store::write_unlock(
                    investment::store::Bank::accountFlags, flag, unlocks::kFlagSet);
            }
        }
        for (const auto& flag : mutation.wrapperFlags) {
            if (!ready) {
                break;
            }
            ready = investment::store::write_unlock(
                investment::store::Bank::accountFlags, flag.index, unlocks::kFlagSet);
        }
        for (std::size_t i = 0; ready && i < mutation.rewardCount; ++i) {
            const auto& reward = mutation.rewards[i];
            const auto& quest = reward.questInitialization;
            if (quest.scope != build_data::items::QuestInitialization::Scope::none
                && reward.previousQuestValue == build_data::items::kUnsetQuestValue) {
                ready = investment::store::write_unlock(quest_bank(quest), quest.row, quest.value);
            }
        }
        ready = ready && transaction.commit();
    }
    if (!ready && mutation.claimedRecordIndex != kUnclaimedRecordIndex) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
        unlocks::records::revoke(mutation.claimedRecordIndex);
    }
    if (ready) {
        report_dropped_rewards(mutation);
    }
    return ready;
}

namespace {

/**
 * Emote definitions seeded into the collection item's wheel lanes.
 * All four are universal Common emotes, so no class or race can reject a seeded lane.
 */
constexpr std::uint32_t kYesEmoteDefinitionHash = 3184938442U;
constexpr std::uint32_t kNopeEmoteDefinitionHash = 48790291U;
constexpr std::uint32_t kCasualSitEmoteDefinitionHash = 383973261U;
constexpr std::uint32_t kCheerEmoteDefinitionHash = 2834933816U;

/** Lane order is the client's own wheel layout. */
constexpr std::array<std::uint32_t, authored_inventory::kEmoteCollectionSocketLaneCount>
    kEmoteCollectionDefaultPlugHashes{
        kCheerEmoteDefinitionHash,     // lane 0 -- top
        kCasualSitEmoteDefinitionHash, // lane 1 -- bottom
        kYesEmoteDefinitionHash,       // lane 2 -- left
        kNopeEmoteDefinitionHash,      // lane 3 -- right
    };

/**
 * Resolves and cross-checks the "Emotes" collection item's own configured content.
 * @param definition Receives the matching native item-definition row.
 * @return True only when both rows agree, declare no native equipment slot, and carry exactly the
 *         expected 4 ordinary socket lanes.
 */
[[nodiscard]] bool
resolve_emote_collection_definition(build_data::items::Definition& definition) noexcept {
    item_details::Definition detail{};
    return build_data::find_item_definition_hash(authored_inventory::kEmoteCollectionDefinitionHash,
                                                 definition)
           && definition.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && detail.bucketId == definition.bucketId && !detail.equipmentSlot.has_value()
           && detail.ordinarySocketState == item_details::OrdinarySocketState::present
           && detail.ordinarySocketCount == authored_inventory::kEmoteCollectionSocketLaneCount;
}

/** Checks every seeded plug is installed and allowed in its lane before an item can carry it. */
[[nodiscard]] bool default_plugs_valid(std::uint16_t collectionDefinitionIndex) noexcept {
    for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
        build_data::items::Definition plugDefinition{};
        if (!build_data::find_item_definition_hash(kEmoteCollectionDefaultPlugHashes[lane],
                                                   plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

/** Checks an equipped collection item's plugs so a stale set is repaired, not trusted. */
[[nodiscard]] bool socket_state_sound(const authored_inventory::Item& item,
                                      std::uint16_t collectionDefinitionIndex) noexcept {
    if (item.sockets.policy != authored_inventory::SocketPolicy::authored
        || item.sockets.plugCount != authored_inventory::kEmoteCollectionSocketLaneCount) {
        return false;
    }
    for (std::size_t lane = 0; lane < authored_inventory::kEmoteCollectionSocketLaneCount; ++lane) {
        const std::optional<std::uint32_t>& plugHash = item.sockets.plugs[lane];
        build_data::items::Definition plugDefinition{};
        if (!plugHash.has_value()
            || !build_data::find_item_definition_hash(*plugHash, plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Equips each character with the "Emotes" collection item in the emote slot.
 * Its content declares no native slot, so every resolver reaches it through
 * resolve_native_equipment_slot. Its 4 lanes have no native default and are seeded here.
 */
EmoteCollectionOutcome ensure_character_emote_collection() noexcept {
    // The wheel occupies the authored emote equipment slot.
    constexpr std::size_t kEmoteCollectionSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::emote);

    // Nothing can be concluded before these domains publish, so this is a retry, not a verdict.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::socket_plug_rules_ready()) {
        return EmoteCollectionOutcome::notReady;
    }
    // Published and still unresolved means the build cannot carry the wheel; a retry never helps.
    build_data::items::Definition collectionDefinition{};
    if (!resolve_emote_collection_definition(collectionDefinition)
        || !default_plugs_valid(collectionDefinition.definitionIndex)) {
        return EmoteCollectionOutcome::unsupported;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::notReady;
    }
    bool changed = false;
    bool failed = false;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount && !failed;
         ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        auto& collectionSlot = character.equipment.slots[kEmoteCollectionSlot];
        const bool present =
            collectionSlot.has_value()
            && collectionSlot->definitionHash == authored_inventory::kEmoteCollectionDefinitionHash;
        if (present && socket_state_sound(*collectionSlot, collectionDefinition.definitionIndex)) {
            continue;
        }
        if (character.nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            failed = true;
            break;
        }
        // A repair owns the definition, the sockets and the serial. Every other field, the
        // item-state flags above all, belongs to the player and survives.
        authored_inventory::Item granted = present ? *collectionSlot : authored_inventory::Item{};
        if (!present) {
            std::uint64_t instanceSoid = 0;
            if (!next_item_instance_soid(candidate, instanceSoid)) {
                failed = true;
                break;
            }
            granted.instanceSoid = instanceSoid;
            granted.level = 0;
            granted.quantity = 1;
        }
        granted.definitionHash = authored_inventory::kEmoteCollectionDefinitionHash;
        granted.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        // Replaced whole: lanes past the used prefix must be empty for the block to validate.
        granted.sockets = authored_inventory::Sockets{};
        granted.sockets.policy = authored_inventory::SocketPolicy::authored;
        granted.sockets.plugCount = kEmoteCollectionDefaultPlugHashes.size();
        for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
            granted.sockets.plugs[lane] = kEmoteCollectionDefaultPlugHashes[lane];
        }
        collectionSlot = granted;
        changed = true;
    }
    if (failed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!changed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::ready;
    }
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    investment::store::g_mutex.unlock();
    return EmoteCollectionOutcome::ready;
}

} // namespace sunrise::state
