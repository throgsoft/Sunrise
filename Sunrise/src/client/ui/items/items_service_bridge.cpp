#include "items_service_bridge.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "../../../core/runtime/wall_clock.h"
#include "../../../server/bap/runtime.h"
#include "../../../server/bap/developer_grant.h"
#include "../../../state/build_data/collectibles/collectible_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/bounty_reward_policy.h"
#include "../../../state/runtime/bright_engram_runtime.h"
#include "../../../state/runtime/developer_investment_runtime.h"
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
        || bucket.bucketId != identity.bucketId) return false;
    const bool engram = identity.bucketId == 31
                        && bucket.arraySelector == buckets::ArraySelector::character;
    const bool consumable = identity.bucketId == 28
                            && bucket.arraySelector == buckets::ArraySelector::profile
                            && detail.instancedDefinitionState
                                   == data::items::details::InstancedDefinitionState::stackable;
    if (!engram && !consumable) return false;
    // The bounty service already admits these concrete engrams, excluding its display wrapper.
    if (engram && detail.instancedDefinitionState
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
        || data::find_season_pass_package(identity.definitionHash, package)) return false;
    const auto count = (std::min)(data::season_pass_reward_count(), data::season_pass::kRewardCapacity);
    for (std::size_t row = 0; row < count; ++row) {
        data::season_pass::Reward reward{};
        if (data::find_season_pass_reward(static_cast<std::uint16_t>(row), reward)
            && reward.quantity > 0 && reward.itemIndex == identity.definitionIndex
            && reward.itemHash == identity.definitionHash) return true;
    }
    return false;
}

Feedback report(const state::developer::Result& result) noexcept {
    // Every service has released SQLite before returning; publication uses the normal protocol.
    if (result.changed) server::bap::request_account_resync();
    Feedback output{result.accepted};
    std::snprintf(output.text.data(), output.text.size(), "%s; changed=%zu",
                  result.reason, result.changed);
    return output;
}

Feedback report_grant(const server::bap::DeveloperGrantReceipt& receipt) noexcept {
    using Status = server::bap::DeveloperGrantStatus;
    Feedback output{};
    output.requestId = receipt.id;
    output.pending = receipt.status == Status::queued || receipt.status == Status::publishing;
    output.accepted = output.pending || receipt.status == Status::published
                      || receipt.status == Status::unchanged;
    if (receipt.status == Status::published)
        std::snprintf(output.text.data(), output.text.size(),
                      "%s; inventory=%zu, Postmaster=%zu, material credits=%zu, F4=%d",
                      receipt.reason, receipt.inventoryCount, receipt.postmasterCount,
                      receipt.materialCount, receipt.family4Version);
    else
        std::snprintf(output.text.data(), output.text.size(), "%s; request=%llu", receipt.reason,
                      static_cast<unsigned long long>(receipt.id));
    return output;
}
} // namespace

Inventory inventory() noexcept {
    Inventory output{};
    try {
        const std::unique_ptr<state::AccountState> account(
            new state::AccountState(state::account_snapshot()));
        if (!state::account::valid(*account)) return output;
        for (std::size_t c = 0; c < (std::min)(account->characterCount, account->characters.size()); ++c) {
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
                if (detail.bucketId != 40 || detail.lifetimeSeconds <= 0 || !detail.objectiveCount
                    || detail.maxStackSize > 1)
                    continue;
                Held held{item.instanceSoid, item.definitionHash, identity.definitionIndex,
                          item.objectiveValues, true, ""};
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
    } catch (...) { return {}; }
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
    const auto brightSource = state::bright_engrams::classify_grant_source(
        identity.definitionIndex, identity.definitionHash);
    if (brightSource == state::bright_engrams::GrantSourceKind::preview) return GrantPolicy::dummy;
    if (brightSource == state::bright_engrams::GrantSourceKind::unsupportedVariant)
        return GrantPolicy::unknown;
    if (brightSource == state::bright_engrams::GrantSourceKind::held) return GrantPolicy::legitimate;
    // Positive installed relationships and existing State payout policy only. An absent dummy
    // flag is never evidence that an arbitrary inventory definition can be minted.
    if (data::collectibles::grants_item(identity.definitionIndex)) return GrantPolicy::legitimate;
    if (direct_runtime_reward(identity, detail)) return GrantPolicy::legitimate;
    if (detail.bucketId == 40 && detail.lifetimeSeconds > 0 && detail.maxStackSize <= 1
        && detail.objectiveCount > 0 && entry.itemType.ends_with("Bounty"))
        return GrantPolicy::legitimate;
    for (const auto& reward : bounty::kScaledPursuitRewards)
        if (reward.paidIndex == identity.definitionIndex) return GrantPolicy::legitimate;
    const auto ingredient = dawning::ingredient(identity.definitionHash);
    if ((ingredient < dawning::kIngredientCount
         && identity.definitionHash == dawning::kIngredients[ingredient].pickupHash)
        || dawning::cookie(identity.definitionHash) || identity.definitionHash == dawning::kEssenceHash)
        return GrantPolicy::legitimate;
    return GrantPolicy::unknown;
}

const char* grant_variant(const Entry& entry) noexcept {
    using Kind = state::bright_engrams::GrantSourceKind;
    switch (state::bright_engrams::classify_grant_source(
        entry.identity.definitionIndex, entry.identity.definitionHash)) {
    case Kind::held: return "Held Bright Engram - grants an instance to the Engrams inventory";
    case Kind::preview: return "Preview dummy - no held instance; granting disabled";
    case Kind::unsupportedVariant: return "Not a supported held Bright Engram - granting disabled";
    case Kind::unrelated: return nullptr;
    }
    return nullptr;
}

Feedback grant(const Entry& entry, std::int32_t quantity) noexcept {
    const auto policy = classify(entry);
    if (policy == GrantPolicy::dummy) return report({false, 0, "Dummy minting is disabled"});
    if (policy != GrantPolicy::legitimate)
        return report({false, 0, "No positive installed grant classification; item is read-only"});
    return report_grant(server::bap::enqueue_developer_item_grant(
        entry.identity.definitionIndex, quantity, entry.identity.definitionHash));
}
Feedback grant_receipt(std::uint64_t requestId) noexcept {
    return report_grant(server::bap::developer_item_grant_receipt(requestId));
}
Feedback set_lane(std::uint64_t instance, std::uint16_t item,
                  std::uint8_t lane, std::int32_t value) noexcept {
    if (lane == 0 || lane > 7) return report({false, 0, "Choose objective lane 1..7"});
    return report(state::developer::set_bounty_lane(instance, item, value, lane));
}
Feedback complete_bounties() noexcept { return report(state::developer::complete_bounties()); }
Feedback clear(Clear category) noexcept {
    switch (category) {
    case Clear::weapons: return report(state::developer::drop_weapons());
    case Clear::armor: return report(state::developer::drop_armor());
    case Clear::bounties: return report(state::developer::drop_bounties());
    case Clear::engrams: return report(state::developer::drop_engrams());
    case Clear::seasonPass: return report(state::developer::drop_season_pass());
    }
    return report({false, 0, "Unknown Clear category"});
}
} // namespace sunrise::client::ui::items::service
