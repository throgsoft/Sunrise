#include <algorithm>
#include <limits>
#include <memory>
#include <new>

#include "../../core/logging/log.h"
#include "bounty_redemption_runtime.h"
#include "bounty_reward_policy_data.h"
#include "bounty_stack_reward_plan.h"
#include "dawning_reward_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::bounty {
namespace inventory = account::inventory;
namespace items = build_data::items;
namespace buckets = build_data::inventory::buckets;

bool stage_rewards(const AccountState& account,
                   std::span<const DirectRecordReward> rewards,
                   PendingRecordRewardGrant& mutation,
                   std::uint64_t reservedSourceSoid,
                   std::int64_t grantTime) noexcept {
    mutation = {};
    const auto characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount)
        return false;
    auto workingStorage = std::unique_ptr<AccountState>{new (std::nothrow) AccountState(account)};
    if (!workingStorage) return false;
    auto& working = *workingStorage;
    std::size_t rewardCount = 0;
    for (const auto& request : rewards) {
        if (request.quantity <= 0) return false;
        PreparedRecordReward material{};
        const auto handled = dawning::stage_reward(request, mutation, material);
        if (handled == dawning::MaterialReward::refused) return false;
        if (handled == dawning::MaterialReward::staged) {
            if (rewardCount == mutation.rewards.size()) return false;
            mutation.rewards[rewardCount++] = material;
            continue;
        }
        items::Definition definition{};
        items::details::Definition detail{};
        buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_index(request.itemDefinitionIndex, definition)
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId || detail.maxStackSize <= 0
            || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket))
            return false;
        // The collector coalesces stack directives before this bounded wire plan is made.
        if (detail.instancedDefinitionState == items::details::InstancedDefinitionState::stackable)
            for (std::size_t i = 0; i < rewardCount; ++i)
                if (mutation.rewards[i].definitionHash == definition.definitionHash) return false;
        if (bucket.arraySelector == buckets::ArraySelector::profile) {
            if (detail.instancedDefinitionState
                    != items::details::InstancedDefinitionState::stackable
                || build_data::is_profile_action_source(definition.definitionIndex,
                                                        definition.bucketId))
                return false;
            std::array<StackRow, inventory::kProfileItemCapacity> matching{};
            std::size_t matchingCount = 0, used = 0;
            std::int32_t serial = 0;
            for (std::size_t i = 0; i < working.profileItemCount; ++i) {
                const auto& row = working.profileItems[i];
                items::Definition held{};
                if (!build_data::find_item_definition_hash(row.definitionHash, held)) return false;
                used += held.bucketId == definition.bucketId;
                serial = (std::max)(serial, row.mutationSerial);
                if (row.definitionHash == definition.definitionHash) {
                    if (row.instanceSoid != 0) return false;
                    matching[matchingCount++] = {i, row.quantity};
                }
            }
            if (used > bucket.slotCount) return false;
            const auto free = (std::min)(working.profileItems.size() - working.profileItemCount,
                                         static_cast<std::size_t>(bucket.slotCount) - used);
            std::array<StackCredit, kRecordRewardGrantCapacity> credits{};
            std::size_t count{};
            std::int32_t credited{};
            const bool multiStack =
                definition.definitionHash == bounty_policy::kEnhancementCoreHash;
            if (!plan_stack_reward(std::span(matching).first(matchingCount),
                                   request.quantity,
                                   detail.maxStackSize,
                                   multiStack,
                                   working.profileItemCount,
                                   free,
                                   std::span(credits).first(mutation.rewards.size() - rewardCount),
                                   count,
                                   credited)
                || count > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()
                                                    - serial))
                return false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& credit = credits[i];
                auto& row = working.profileItems[credit.index];
                if (credit.appended) {
                    row = {0, definition.definitionHash, credit.after, ++serial};
                    ++working.profileItemCount;
                } else {
                    row.quantity = credit.after;
                    row.mutationSerial = ++serial;
                }
                auto& reward = mutation.rewards[rewardCount++];
                reward.definitionHash = definition.definitionHash;
                reward.stateIndex = credit.index;
                reward.quantity = credit.credited;
                reward.afterQuantity = credit.after;
                reward.mutationSerial = row.mutationSerial;
                reward.kind = RecordRewardKind::profileStack;
            }
            if (credited != request.quantity) {
                // Retain the prior saturation policy. No Postmaster resident is established.
                core::log::writef(
                    core::log::Channel::state,
                    core::log::Level::info,
                    "ev=bounty_reward stage=capacity item=%u requested=%d credited=%d "
                    "postmaster_queued=0",
                    static_cast<unsigned>(definition.definitionIndex),
                    request.quantity,
                    credited);
            }
        } else if (bucket.arraySelector == buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == items::details::InstancedDefinitionState::instanced) {
            if (request.quantity != 1 || rewardCount == mutation.rewards.size()) return false;
            PendingItemAcquisition acquired{};
            if (!finalize_item_acquisition(
                    working, working, definition.definitionHash, false, {.direct = true}, acquired))
                return false;
            auto& resident = acquired.afterCharacter.inventory.values[acquired.inventoryIndex];
            if (resident.instanceSoid <= reservedSourceSoid) {
                if (reservedSourceSoid == (std::numeric_limits<std::uint64_t>::max)()) return false;
                auto soid = reservedSourceSoid + 1;
                while (account_owns_soid(working, soid)) {
                    if (soid == (std::numeric_limits<std::uint64_t>::max)()) return false;
                    ++soid;
                }
                resident.instanceSoid = acquired.acquiredInstanceSoid = soid;
            }
            if (detail.objectiveCount != 0 && detail.lifetimeSeconds > 0 && grantTime > 0) {
                if (grantTime > (std::numeric_limits<std::int32_t>::max)() - detail.lifetimeSeconds)
                    return false;
                resident.objectiveValues[inventory::kItemExpiryLane] =
                    static_cast<std::int32_t>(grantTime + detail.lifetimeSeconds);
            }
            working.characters[characterIndex] = acquired.afterCharacter;
            auto& reward = mutation.rewards[rewardCount++];
            reward.instanceSoid = acquired.acquiredInstanceSoid;
            reward.definitionHash = definition.definitionHash;
            reward.stateIndex = acquired.inventoryIndex;
            reward.quantity = reward.afterQuantity = 1;
            reward.mutationSerial =
                acquired.afterCharacter.inventory.values[acquired.inventoryIndex].mutationSerial;
            reward.inventoryRow = acquired.inventoryRow;
            reward.kind = RecordRewardKind::characterInstance;
        } else if (bucket.arraySelector == buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == items::details::InstancedDefinitionState::stackable
                   && !detail.equipmentSlot.has_value()) {
            auto& character = working.characters[characterIndex];
            std::size_t index = character.stacks.count;
            for (std::size_t i = 0; i < character.stacks.count; ++i) {
                if (character.stacks.values[i].definitionHash == definition.definitionHash) {
                    index = i;
                    break;
                }
            }
            const bool append = index == character.stacks.count;
            const auto before = append ? 0 : character.stacks.values[index].quantity;
            if (before < 0 || before > detail.maxStackSize) return false;
            const auto credited = (std::min)(request.quantity, detail.maxStackSize - before);
            if (credited == 0) continue;
            if (rewardCount == mutation.rewards.size()
                || (append && index == character.stacks.values.size())
                || character.nextInventorySerial
                       >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
                return false;
            auto& stack = character.stacks.values[index];
            stack.definitionHash = definition.definitionHash;
            stack.quantity = before + credited;
            stack.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
            if (append) ++character.stacks.count;
            auto& reward = mutation.rewards[rewardCount++];
            reward.definitionHash = definition.definitionHash;
            reward.stateIndex = index;
            reward.quantity = credited;
            reward.afterQuantity = stack.quantity;
            reward.mutationSerial = stack.mutationSerial;
            reward.kind = RecordRewardKind::characterStack;
        } else
            return false;
    }
    middleware::datagen::family4::loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !middleware::datagen::family4::loadout::resolve(working, characterIndex, loadout))
        return false;
    for (std::size_t i = 0; i < rewardCount; ++i) {
        auto& reward = mutation.rewards[i];
        if (reward.kind == RecordRewardKind::characterInstance) {
            std::uint8_t slot{};
            if (!find_unequipped_row(loadout, reward.instanceSoid, reward.inventoryRow, slot))
                return false;
        }
    }
    mutation.beforeCharacter = account.characters[characterIndex];
    mutation.afterCharacter = working.characters[characterIndex];
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = working.profileItems;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.beforeProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = working.profileItemCount;
    mutation.rewardCount = rewardCount;
    mutation.prepared = true;
    return true;
}
} // namespace sunrise::state::runtime::detail::bounty
