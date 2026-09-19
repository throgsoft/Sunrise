/**
 * Grants that no purchase pays for: season pass rewards, record rewards, and the
 * default emote collection.
 */
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../unlocks/unlocks_records.h"
#include "bounty_redemption_runtime.h"
#include "character_encoding_preflight.h"
#include "dawning_reward_runtime.h"
#include "fifo_bucket_eviction.h"
#include "profile_stack_credit.h"
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

[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept;

/**
 * Checks that a staged grant is the one the authored reward row describes.
 * @param reward Authored season pass reward row.
 * @return False when the grant's item, quantity or bundle does not match the row.
 */
[[nodiscard]] bool reward_matches(const build_data::season_pass::Reward& reward,
                                  const PendingSeasonPassReward& mutation) noexcept {
    if (!mutation.prepared || mutation.sourceDefinitionHash != reward.itemHash) {
        return false;
    }
    if (const auto* item = std::get_if<PendingItemAcquisition>(&mutation.grant)) {
        if (reward.quantity != 1) {
            return false;
        }
        if (reward.itemHash != progression::season_pass::kLegendaryEngramHash
            && reward.itemHash != progression::season_pass::kExoticEngramHash) {
            return item->acquiredDefinitionHash == reward.itemHash;
        }
        return progression::season_pass::contains_engram_reward(
            reward.itemHash,
            item->acquiredDefinitionHash,
            static_cast<std::uint8_t>(item->afterCharacter.characterClass));
    }
    if (const auto* profile = std::get_if<PendingProfileItemAcquisition>(&mutation.grant)) {
        // A wallet row saturates at its cap, so the claim credits at most what the row asked for.
        const auto credited = profile->acquiredQuantity - profile->previousQuantity;
        return profile->acquiredDefinitionHash == reward.itemHash && credited > 0
               && credited <= static_cast<std::int32_t>(reward.quantity);
    }
    if (const auto* bundle = std::get_if<PendingDirectItemBundle>(&mutation.grant)) {
        build_data::season_pass::Package package{};
        return reward.quantity == 1 && bundle->sourceDefinitionHash == reward.itemHash
               && build_data::find_season_pass_package(reward.itemHash, package);
    }
    const auto* resources = std::get_if<PendingRecordRewardGrant>(&mutation.grant);
    if (resources == nullptr || resources->rewardCount == 0
        || resources->rewardCount > resources->rewards.size()) {
        return false;
    }
    if (reward.itemHash == progression::season_pass::kDestinationResourceBundleHash) {
        if (reward.quantity != 1
            || resources->rewardCount
                   != progression::season_pass::kDestinationResourceHashes.size()) {
            return false;
        }
        for (std::size_t index = 0; index < resources->rewardCount; ++index) {
            if (resources->rewards[index].definitionHash
                    != progression::season_pass::kDestinationResourceHashes[index]
                || resources->rewards[index].quantity
                       != progression::season_pass::kDestinationResourceQuantity) {
                return false;
            }
        }
        return true;
    }
    // One stackable reward may credit several rows and may saturate, so every row has to be the
    // reward's own item and the total may not exceed what the row promised.
    std::int64_t credited = 0;
    for (std::size_t index = 0; index < resources->rewardCount; ++index) {
        if (resources->rewards[index].definitionHash != reward.itemHash) return false;
        credited += resources->rewards[index].quantity;
    }
    return credited > 0 && credited <= static_cast<std::int64_t>(reward.quantity);
}

} // namespace

/** Commits a reward and its claim together after every outbound byte has been staged. */
bool commit_season_pass_reward(PendingSeasonPassReward& mutation) noexcept {
    const PendingConsumption consume{mutation};
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(mutation.rewardIndex, reward)
        || !reward_matches(reward, mutation)) {
        revoke_season_pass_reward(mutation.rewardIndex);
        return false;
    }

    const bool ready = [&]() noexcept {
        investment::store::Transaction transaction;
        if (!transaction.ready()) return false;
        AccountState after{};
        bool staged = false;
        if (const auto* item = std::get_if<PendingItemAcquisition>(&mutation.grant)) {
            staged = materialize_item_acquisition(investment::store::account(), *item, after);
        } else if (const auto* profile =
                       std::get_if<PendingProfileItemAcquisition>(&mutation.grant)) {
            staged = materialize_profile_acquisition(investment::store::account(), *profile, after);
        } else if (const auto* bundle = std::get_if<PendingDirectItemBundle>(&mutation.grant)) {
            staged = materialize_direct_item_bundle(investment::store::account(), *bundle, after);
        } else if (const auto* resources = std::get_if<PendingRecordRewardGrant>(&mutation.grant)) {
            staged = materialize_record_reward(investment::store::account(), *resources, after)
                     && dawning::write_rewards(*resources);
        }
        return staged && investment::store::write_account(after) && transaction.commit();
    }();
    if (!ready) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
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
    if (mutation.pursuitRedemption)
        return bounty::materialize_redemption_grant(current, mutation, after);
    if (!dawning::validate_rewards(mutation) || !mutation.prepared || mutation.rewardCount == 0
        || mutation.rewardCount > mutation.rewards.size() || mutation.accountSoid == 0
        || mutation.characterSoid == 0 || mutation.characterIndex >= current.characterCount
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
        || !family4_loadout::resolve(after, mutation.characterIndex, loadout)
        || !character_encoding_preflight(
            after,
            mutation.characterIndex,
            loadout,
            mutation.afterCharacter.inventory.count > mutation.beforeCharacter.inventory.count
                || mutation.afterCharacter.stacks.count > mutation.beforeCharacter.stacks.count)) {
        return false;
    }

    for (std::size_t index = 0; index < mutation.rewardCount; ++index) {
        const PreparedRecordReward& reward = mutation.rewards[index];
        if (reward.kind == RecordRewardKind::accountMaterial) continue;
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (reward.definitionHash == authored_inventory::kNoDefinitionHash || reward.quantity <= 0
            || reward.afterQuantity < reward.quantity || reward.mutationSerial < 0
            || !build_data::find_item_definition_hash(reward.definitionHash, item)
            || !build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
            return false;
        }
        if (reward.kind == RecordRewardKind::characterInstance) {
            if (reward.quantity != 1 || reward.afterQuantity != 1 || reward.instanceSoid == 0
                || reward.appendedProfileResident
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::instanced
                || bucket.arraySelector != inventory_buckets::ArraySelector::character
                || reward.stateIndex >= mutation.afterCharacter.inventory.count) {
                return false;
            }
            const auto& granted = mutation.afterCharacter.inventory.values[reward.stateIndex];
            std::uint16_t row = 0;
            std::uint8_t slot = 0;
            if (granted.instanceSoid != reward.instanceSoid
                || granted.definitionHash != reward.definitionHash || granted.quantity != 1
                || granted.mutationSerial != reward.mutationSerial
                || !find_unequipped_row(loadout, reward.instanceSoid, row, slot)
                || row != reward.inventoryRow) {
                return false;
            }
        } else if (reward.kind == RecordRewardKind::characterStack) {
            if (reward.instanceSoid != 0 || reward.appendedProfileResident
                || detail.equipmentSlot.has_value()
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::stackable
                || bucket.arraySelector != inventory_buckets::ArraySelector::character
                || reward.afterQuantity > detail.maxStackSize
                || reward.stateIndex >= mutation.afterCharacter.stacks.count) {
                return false;
            }
            const auto& granted = mutation.afterCharacter.stacks.values[reward.stateIndex];
            if (granted.definitionHash != reward.definitionHash
                || granted.quantity != reward.afterQuantity
                || granted.mutationSerial != reward.mutationSerial) {
                return false;
            }
        } else {
            if (reward.kind != RecordRewardKind::profileStack
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::stackable
                || bucket.arraySelector != inventory_buckets::ArraySelector::profile
                || reward.afterQuantity > detail.maxStackSize
                || reward.stateIndex >= mutation.afterProfileItemCount) {
                return false;
            }
            const auto& granted = mutation.afterProfileItems[reward.stateIndex];
            const bool actionSource =
                build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
            if (granted.instanceSoid != reward.instanceSoid
                || granted.definitionHash != reward.definitionHash
                || granted.quantity != reward.afterQuantity
                || granted.mutationSerial != reward.mutationSerial
                || actionSource != (reward.instanceSoid != 0)
                || reward.appendedProfileResident
                       != (actionSource && reward.stateIndex >= mutation.beforeProfileItemCount)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** Prepares every reward over one cumulative account view. */
bool runtime::detail::stage_record_reward_grant(const AccountState& account,
                                                std::span<const DirectRecordReward> rewards,
                                                std::uint16_t claimedRecordIndex,
                                                PendingRecordRewardGrant& mutation) noexcept {
    mutation = {};
    if (rewards.empty() || rewards.size() > mutation.rewards.size()) {
        return false;
    }
    const std::size_t characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount) {
        return false;
    }

    AccountState working = account;
    std::size_t rewardCount = 0;
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const DirectRecordReward& requested = rewards[index];
        PreparedRecordReward material{};
        const auto materialResult = dawning::stage_reward(
            working.characters[characterIndex], requested, mutation, material);
        if (materialResult == dawning::MaterialReward::refused) return false;
        if (materialResult == dawning::MaterialReward::staged) {
            if (rewardCount >= mutation.rewards.size()) return false;
            mutation.rewards[rewardCount++] = material;
            continue;
        }
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requested.quantity <= 0
            || !build_data::find_item_definition_index(requested.itemDefinitionIndex, item)
            || !build_data::find_configured_item_detail(requested.itemDefinitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
            return false;
        }
        if (detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable) {
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (mutation.rewards[prior].definitionHash == item.definitionHash) {
                    return false;
                }
            }
        }

        PreparedRecordReward prepared{};
        prepared.definitionHash = item.definitionHash;
        prepared.quantity = requested.quantity;
        if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
            // The same planner the earned reward path uses: saturate a full row, spread to
            // another while the bucket owns a slot, and report every row it credited.
            std::int32_t credited = 0;
            if (!bounty::credit_profile_stacks(working,
                                               item,
                                               detail,
                                               bucket,
                                               requested.quantity,
                                               mutation,
                                               rewardCount,
                                               credited)) {
                return false;
            }
            continue;
        } else if (bucket.arraySelector == inventory_buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == item_details::InstancedDefinitionState::instanced) {
            if (requested.quantity != 1) {
                return false;
            }
            PendingItemAcquisition staged{};
            if (!finalize_item_acquisition(
                    working, working, item.definitionHash, false, {.direct = true}, staged)) {
                return false;
            }
            working.characters[characterIndex] = staged.afterCharacter;
            prepared.instanceSoid = staged.acquiredInstanceSoid;
            prepared.stateIndex = staged.inventoryIndex;
            prepared.afterQuantity = 1;
            prepared.mutationSerial =
                staged.afterCharacter.inventory.values[staged.inventoryIndex].mutationSerial;
            prepared.inventoryRow = staged.inventoryRow;
            prepared.kind = RecordRewardKind::characterInstance;
        } else if (bucket.arraySelector == inventory_buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == item_details::InstancedDefinitionState::stackable
                   && !detail.equipmentSlot.has_value()) {
            CharacterState& character = working.characters[characterIndex];
            if (character.nextInventorySerial
                >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
                return false;
            }
            // The installed policy admits into a full first-in-first-out bucket by dropping its
            // oldest row rather than refusing the arrival.
            {
                std::size_t occupied = 0;
                bool matched = false;
                for (std::size_t candidate = 0; candidate < character.stacks.count; ++candidate) {
                    const auto& row = character.stacks.values[candidate];
                    build_data::items::Definition held{};
                    if (!build_data::find_item_definition_hash(row.definitionHash, held)) {
                        return false;
                    }
                    occupied += held.bucketId == item.bucketId;
                    matched = matched || row.definitionHash == item.definitionHash;
                }
                if (!matched && occupied >= bucket.slotCount) {
                    (void)evict_oldest_stacks(character, bucket, occupied - bucket.slotCount + 1);
                }
            }
            std::size_t stackIndex = character.stacks.count;
            std::size_t bucketStacks = 0;
            for (std::size_t candidate = 0; candidate < character.stacks.count; ++candidate) {
                const auto& row = character.stacks.values[candidate];
                build_data::items::Definition held{};
                if (!build_data::find_item_definition_hash(row.definitionHash, held)) return false;
                bucketStacks += held.bucketId == item.bucketId;
                if (row.definitionHash == item.definitionHash) stackIndex = candidate;
            }
            const bool appended = stackIndex == character.stacks.count;
            const std::int32_t held = appended ? 0 : character.stacks.values[stackIndex].quantity;
            if (held < 0 || held > detail.maxStackSize) return false;
            // The same saturation the earned reward path uses: credit what the stack can hold
            // rather than refusing the payout, and keep a bucket inside its own slot range.
            const std::int32_t credited =
                (std::min)(requested.quantity, detail.maxStackSize - held);
            if (credited <= 0 || (appended && stackIndex >= character.stacks.values.size())
                || (appended && bucketStacks >= bucket.slotCount)) {
                return false;
            }
            auto& stack = character.stacks.values[stackIndex];
            if (appended) {
                stack.definitionHash = item.definitionHash;
                ++character.stacks.count;
            }
            stack.quantity = held + credited;
            stack.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
            prepared.quantity = credited;
            prepared.stateIndex = stackIndex;
            prepared.afterQuantity = stack.quantity;
            prepared.mutationSerial = stack.mutationSerial;
            prepared.kind = RecordRewardKind::characterStack;
        } else {
            return false;
        }
        if (rewardCount >= mutation.rewards.size()) return false;
        mutation.rewards[rewardCount++] = prepared;
    }

    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)
        || !character_encoding_preflight(
            working,
            characterIndex,
            loadout,
            working.characters[characterIndex].inventory.count
                    > account.characters[characterIndex].inventory.count
                || working.characters[characterIndex].stacks.count
                       > account.characters[characterIndex].stacks.count)) {
        return false;
    }
    mutation.beforeCharacter = account.characters[characterIndex];
    mutation.afterCharacter = working.characters[characterIndex];
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = working.profileItems;
    mutation.claimedRecordIndex = claimedRecordIndex;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.beforeProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = working.profileItemCount;
    mutation.rewardCount = rewardCount;
    mutation.prepared = true;
    return true;
}

bool prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                 std::uint16_t claimedRecordIndex,
                                 PendingRecordRewardGrant& mutation) noexcept {
    return stage_record_reward_grant(account_snapshot(), rewards, claimedRecordIndex, mutation);
}

bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    after = {};
    return materialize_record_reward(account_snapshot(), mutation, after);
}

/** Commits the shared reward after-image and claim together. */
bool commit_record_reward(PendingRecordRewardGrant& mutation) noexcept {
    const PendingConsumption consume{mutation};
    if (mutation.pursuitRedemption) return bounty::commit_redemption_grant(mutation);
    const bool ready = [&]() noexcept {
        investment::store::Transaction transaction;
        AccountState after{};
        // A quest handed out as a reward needs its first step seeded, exactly as the acquisition
        // path does, or it lands with no active step and the Client cannot track it.
        const auto seedQuests = [&mutation]() noexcept {
            for (std::size_t i = 0; i < mutation.rewardCount && i < mutation.rewards.size(); ++i)
                if (!seed_quest_initialization(mutation.rewards[i].definitionHash)) return false;
            return true;
        };
        return transaction.ready()
               && materialize_record_reward(investment::store::account(), mutation, after)
               && dawning::write_rewards(mutation) && investment::store::write_account(after)
               && seedQuests() && transaction.commit();
    }();
    if (!ready && mutation.claimedRecordIndex != kUnclaimedRecordIndex) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
        unlocks::records::revoke(mutation.claimedRecordIndex);
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
