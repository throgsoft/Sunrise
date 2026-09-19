#include "items_service_bridge.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "../../../core/runtime/wall_clock.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/build_data/collectibles/collectible_catalog.h"
#include "../../../state/build_data/items/quest_initialization.h"
#include "../../../state/build_data/pursuits/pursuit_progress.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/bounty_reward_policy.h"
#include "../../../state/runtime/investment_edit_runtime.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::client::ui::items::service {
namespace {
namespace data = state::build_data;

bool direct_runtime_reward(const data::items::Definition& identity,
                           const data::items::details::Definition& detail) noexcept {
    namespace pass = state::progression::season_pass;
    namespace buckets = data::inventory::buckets;
    namespace bounty = state::runtime::detail::bounty;
    namespace bountyPolicy = state::runtime::detail::bounty_policy;
    buckets::Descriptor bucket{};
    if (detail.equipmentSlot || detail.objectiveCount || detail.maxStackSize <= 0
        || !data::find_inventory_bucket_descriptor(identity.bucketId, bucket)
        || bucket.bucketId != identity.bucketId)
        return false;
    const bool engram = identity.bucketId == buckets::kEngramBucketId
                        && bucket.arraySelector == buckets::ArraySelector::character;
    // Materials sit in their own profile bucket beside consumables; both hold granted resources.
    const bool consumable = (identity.bucketId == buckets::kConsumableBucketId
                             || identity.bucketId == buckets::kMaterialBucketId)
                            && bucket.arraySelector == buckets::ArraySelector::profile
                            && detail.instancedDefinitionState
                                   == data::items::details::InstancedDefinitionState::stackable;
    if (!engram && !consumable) return false;
    // The bounty service already admits these concrete engrams, excluding its display wrapper.
    if (engram
        && detail.instancedDefinitionState
               == data::items::details::InstancedDefinitionState::instanced
        && bounty::contains(bountyPolicy::kArrivalsUmbralEngrams, identity.definitionHash))
        return true;
    if (consumable && pass::contains(pass::kDestinationResourceHashes, identity.definitionHash))
        return true;
    // Match the pass's direct-acquisition branch, never its bundle or auto-decrypt branches.
    data::season_pass::Package package{};
    if (identity.definitionHash == pass::kDestinationResourceBundleHash
        || identity.definitionHash == pass::kLegendaryEngramHash
        || identity.definitionHash == pass::kExoticEngramHash
        || data::find_season_pass_package(identity.definitionHash, package))
        return false;
    const auto count =
        (std::min)(data::season_pass_reward_count(), data::season_pass::kRewardCapacity);
    for (std::size_t row = 0; row < count; ++row) {
        data::season_pass::Reward reward{};
        if (data::find_season_pass_reward(static_cast<std::uint16_t>(row), reward)
            && reward.quantity > 0 && reward.itemIndex == identity.definitionIndex
            && reward.itemHash == identity.definitionHash)
            return true;
    }
    return false;
}

Feedback report(const state::investment_edit::Result& result) noexcept {
    // Every service has released SQLite before returning; publication uses the normal protocol.
    if (result.changed) server::bap::request_account_resync();
    Feedback output{result.accepted};
    std::snprintf(
        output.text.data(), output.text.size(), "%s; changed=%zu", result.reason, result.changed);
    return output;
}

} // namespace

Inventory inventory() noexcept {
    Inventory output{};
    try {
        const std::unique_ptr<state::AccountState> account(
            new state::AccountState(state::account_snapshot()));
        if (!state::account::valid(*account)) return output;
        for (std::size_t c = 0; c < (std::min)(account->characterCount, account->characters.size());
             ++c) {
            const auto& character = account->characters[c];
            if (!character.selected) continue;
            output.character = character.soid;
            output.ready = true;
            for (std::size_t i = 0; i < character.inventory.count; ++i) {
                const auto& item = character.inventory.values[i];
                data::items::Definition identity{};
                data::items::details::Definition detail{};
                if (!data::find_item_definition_hash(item.definitionHash, identity)
                    || !data::find_configured_item_detail(identity.definitionIndex, detail)
                    || detail.definitionHash != identity.definitionHash
                    || detail.bucketId != identity.bucketId) {
                    ++output.unresolved;
                    continue;
                }
                if (detail.bucketId != data::items::kPursuitBucketId || detail.lifetimeSeconds <= 0
                    || !detail.objectiveCount || detail.maxStackSize > 1)
                    continue;
                Held held{item.instanceSoid,
                          item.definitionHash,
                          identity.definitionIndex,
                          item.objectiveValues,
                          true,
                          ""};
                if (item.objectiveDefinitionIndex != 0xFFFF
                    && item.objectiveDefinitionIndex != identity.definitionIndex) {
                    held.editable = false;
                    held.reason = "Saved objective definition differs from installed item.";
                } else if (held.values[0] == 0
                           || held.values[0] <= core::runtime::investment_clock_seconds()) {
                    held.editable = false;
                    held.reason = "Expired bounty or missing saved expiry.";
                }
                output.bounties.push_back(held);
            }
            break;
        }
    } catch (...) {
        return {};
    }
    return output;
}

GrantPolicy classify(const Entry& entry) noexcept {
    namespace bounty = state::runtime::detail::bounty;
    namespace dawning = state::account::inventory::dawning;
    data::items::Definition identity{};
    data::items::details::Definition detail{};
    if (!data::find_item_definition_hash(entry.identity.definitionHash, identity)
        || identity.definitionIndex != entry.identity.definitionIndex
        || identity.bucketId != entry.identity.bucketId
        || !data::find_configured_item_detail(identity.definitionIndex, detail)
        || detail.definitionHash != identity.definitionHash || detail.bucketId != identity.bucketId)
        return GrantPolicy::unknown;
    if (bounty::reward_marker(identity.definitionIndex, identity.definitionHash)
        != bounty::RewardMarker::none)
        return GrantPolicy::dummy;
    // Positive installed relationships and existing State payout policy only. An absent dummy
    // flag is never evidence that an arbitrary inventory definition can be minted.
    if (data::collectibles::grants_item(identity.definitionIndex)) return GrantPolicy::legitimate;
    if (direct_runtime_reward(identity, detail)) return GrantPolicy::legitimate;
    // Bounties expire and quests do not, but both are pursuits and the grant seeds a quest's
    // authored first step, so the structural shape decides rather than the localized type name.
    if (detail.bucketId == data::items::kPursuitBucketId && detail.maxStackSize <= 1
        && detail.objectiveCount > 0)
        return GrantPolicy::legitimate;
    for (const auto& reward : bounty::kScaledPursuitRewards)
        if (reward.paidIndex == identity.definitionIndex) return GrantPolicy::legitimate;
    const auto ingredient = dawning::ingredient(identity.definitionHash);
    if ((ingredient < dawning::kIngredientCount
         && identity.definitionHash == dawning::kIngredients[ingredient].pickupHash)
        || dawning::cookie(identity.definitionHash)
        || identity.definitionHash == dawning::kEssenceHash)
        return GrantPolicy::legitimate;
    return GrantPolicy::unknown;
}

Feedback grant(const Entry& entry, std::int32_t quantity) noexcept {
    const auto policy = classify(entry);
    if (policy == GrantPolicy::dummy) return report({false, 0, "Dummy minting is disabled"});
    if (policy != GrantPolicy::legitimate)
        return report({false, 0, "No positive installed grant classification; item is read-only"});
    return report(state::investment_edit::grant_item(
        entry.identity.definitionIndex, quantity, entry.identity.definitionHash));
}
Feedback set_lane(std::uint64_t instance,
                  std::uint16_t item,
                  std::uint8_t lane,
                  std::int32_t value) noexcept {
    if (lane == 0 || lane > 7) return report({false, 0, "Choose objective lane 1..7"});
    return report(state::investment_edit::set_objective_lane(instance, item, value, lane));
}
Feedback complete_bounties() noexcept {
    return report(state::investment_edit::complete_bounties());
}

/** An installed bounty: a character-bucket pursuit that expires. */
bool installed_bounty(std::uint16_t index,
                      data::items::Definition& item,
                      data::items::details::Definition& detail) noexcept {
    data::inventory::buckets::Descriptor bucket{};
    return data::find_item_definition_index(index, item)
           && data::find_configured_item_detail(index, detail)
           && detail.definitionHash == item.definitionHash && detail.bucketId == item.bucketId
           && detail.objectiveCount != 0 && detail.objectiveCount <= detail.objectiveIndices.size()
           && !detail.equipmentSlot.has_value() && detail.maxStackSize <= 1
           && detail.bucketId == data::items::kPursuitBucketId && detail.lifetimeSeconds > 0
           && data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
           && bucket.arraySelector == data::inventory::buckets::ArraySelector::character;
}

Pages bounty_pages() noexcept {
    const auto definitions = (std::min)(data::item_definition_count(), std::size_t{65536});
    Pages pages{};
    for (std::size_t i = 0; i < definitions; ++i) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        pages.bounties += installed_bounty(static_cast<std::uint16_t>(i), item, detail);
    }
    pages.count = (pages.bounties + kBountyPageSize - 1) / kBountyPageSize;
    return pages;
}

Feedback grant_bounty_page(std::size_t page) noexcept {
    const auto pages = bounty_pages();
    if (page == 0 || page > pages.count)
        return report({false, 0, "Page is outside the installed bounty range"});
    // Discard first so a reused resident cannot be mistaken for one this page granted.
    const auto dropped = state::investment_edit::drop_bounties();
    if (!dropped.accepted) return report(dropped);
    const auto account = state::account_snapshot();
    const state::CharacterState* character = nullptr;
    for (std::size_t c = 0; c < account.characterCount; ++c)
        if (account.characters[c].selected) character = &account.characters[c];
    if (!character) {
        if (dropped.changed != 0) server::bap::request_account_resync();
        return report({false, dropped.changed, "No selected character"});
    }
    const auto definitions = (std::min)(data::item_definition_count(), std::size_t{65536});
    const auto first = (page - 1) * kBountyPageSize;
    std::size_t ordinal = 0, changed = dropped.changed, granted = 0, reused = 0, refused = 0;
    for (std::size_t i = 0; i < definitions && ordinal < first + kBountyPageSize; ++i) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!installed_bounty(static_cast<std::uint16_t>(i), item, detail)) continue;
        if (ordinal++ < first) continue;
        bool held = false, heldComplete = true;
        for (std::size_t row = 0; row < character->inventory.count; ++row) {
            const auto& resident = character->inventory.values[row];
            if (resident.definitionHash != item.definitionHash) continue;
            held = true;
            heldComplete =
                heldComplete
                && data::pursuits::complete(item.definitionIndex, resident.objectiveValues);
        }
        if (held && heldComplete) {
            ++reused;
            continue;
        }
        if (!held)
            (void)state::investment_edit::grant_item(item.definitionIndex, 1, item.definitionHash);
        const auto result =
            state::investment_edit::complete_bounty(item.definitionIndex, item.definitionHash);
        changed += result.changed;
        granted += result.accepted && !held;
        reused += held;
        refused += !result.accepted;
    }
    if (changed != 0) server::bap::request_account_resync();
    Feedback output{refused == 0};
    std::snprintf(output.text.data(),
                  output.text.size(),
                  "page %zu/%zu: %zu granted, %zu reused, %zu refused",
                  page,
                  pages.count,
                  granted,
                  reused,
                  refused);
    return output;
}

Feedback page_bounty(const Entry& entry) noexcept {
    if (!entry.bounty) return report({false, 0, "Not an installed bounty"});
    (void)state::investment_edit::grant_item(
        entry.identity.definitionIndex, 1, entry.identity.definitionHash);
    return report(state::investment_edit::complete_bounty(entry.identity.definitionIndex,
                                                          entry.identity.definitionHash));
}
Feedback clear(Clear category) noexcept {
    switch (category) {
    case Clear::weapons:
        return report(state::investment_edit::drop_weapons());
    case Clear::armor:
        return report(state::investment_edit::drop_armor());
    case Clear::bounties:
        return report(state::investment_edit::drop_bounties());
    case Clear::engrams:
        return report(state::investment_edit::drop_engrams());
    case Clear::seasonPass:
        return report(state::investment_edit::drop_season_pass());
    }
    return report({false, 0, "Unknown Clear category"});
}
} // namespace sunrise::client::ui::items::service
