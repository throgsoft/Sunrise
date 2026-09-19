#include "items_service_bridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

#include "../../../core/runtime/wall_clock.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/build_data/items/quest_initialization.h"
#include "../../../state/build_data/pursuits/pursuit_progress.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/bounty_reward_policy.h"
#include "../../../state/runtime/investment_edit_runtime.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::client::ui::items::service {
namespace {
namespace data = state::build_data;

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
                          item.mutationSerial,
                          data::pursuits::complete(identity.definitionIndex, item.objectiveValues),
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
            // Inventory order follows acquisition and compaction, not presentation. The Character
            // screen floats finished pursuits so the player knows to redeem them, and orders the
            // rest by the serial it sorts a bucket with, most recently acquired first.
            std::sort(output.bounties.begin(),
                      output.bounties.end(),
                      [](const Held& left, const Held& right) noexcept {
                          if (left.complete != right.complete) return left.complete;
                          return left.mutationSerial != right.mutationSerial
                                     ? left.mutationSerial > right.mutationSerial
                                     : left.instance > right.instance;
                      });
            break;
        }
    } catch (...) {
        return {};
    }
    return output;
}

GrantPolicy classify(const Entry& entry) noexcept {
    namespace bounty = state::runtime::detail::bounty;
    namespace buckets = data::inventory::buckets;
    data::items::Definition identity{};
    data::items::details::Definition detail{};
    buckets::Descriptor bucket{};
    if (!data::find_item_definition_hash(entry.identity.definitionHash, identity)
        || identity.definitionIndex != entry.identity.definitionIndex
        || identity.bucketId != entry.identity.bucketId
        || !data::find_configured_item_detail(identity.definitionIndex, detail)
        || detail.definitionHash != identity.definitionHash || detail.bucketId != identity.bucketId
        || !data::find_inventory_bucket_descriptor(identity.bucketId, bucket)
        || bucket.bucketId != identity.bucketId)
        return GrantPolicy::unknown;
    // A reward marker stands in for an item without ever being a resident of its own.
    if (bounty::reward_marker(identity.definitionIndex, identity.definitionHash)
        != bounty::RewardMarker::none)
        return GrantPolicy::dummy;
    // A bucket that cannot transfer what it evicts is a delivery lane, not somewhere an item
    // lives: acquiring from it is a side effect elsewhere, such as a weapon gaining a plug.
    // Minting the row would show an item the account does not really own.
    if ((bucket.policyFlags & buckets::kNoTransferOnEviction) != 0) return GrantPolicy::unknown;
    // Reward policy owns support, ownership, capacity and quantity, and refuses with its own
    // reason. This mirrors only the placements that policy implements, so a definition naming
    // no bucket it can fill never offers a grant it cannot honour.
    const bool stackable = detail.instancedDefinitionState
                           == data::items::details::InstancedDefinitionState::stackable;
    switch (bucket.arraySelector) {
    case buckets::ArraySelector::profile:
        if (identity.rollSetIndex != data::items::kNoRollSet
            && !data::is_profile_action_source(identity.definitionIndex, identity.bucketId)) {
            return GrantPolicy::unknown;
        }
        return stackable ? GrantPolicy::legitimate : GrantPolicy::unknown;
    case buckets::ArraySelector::character:
        return !stackable || !detail.equipmentSlot.has_value() ? GrantPolicy::legitimate
                                                               : GrantPolicy::unknown;
    default:
        return GrantPolicy::unknown;
    }
}

Feedback grantable(const Entry& entry) noexcept {
    Feedback output{};
    if (classify(entry) == GrantPolicy::dummy) {
        std::snprintf(output.text.data(),
                      output.text.size(),
                      "Reward marker; it stands for an item without being one");
        return output;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> probe(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!probe) {
        std::snprintf(output.text.data(), output.text.size(), "Probe allocation failed");
        return output;
    }
    const std::array rewards{state::DirectRecordReward{entry.identity.definitionIndex, 1}};
    output.accepted =
        state::prepare_record_reward_grant(rewards, state::kUnclaimedRecordIndex, *probe);
    if (!output.accepted) std::snprintf(output.text.data(), output.text.size(), "Unable to grant");
    return output;
}

Feedback grant(const Entry& entry, std::int32_t quantity) noexcept {
    const auto policy = classify(entry);
    if (policy == GrantPolicy::dummy)
        return report({false, 0, "Reward marker; it stands for an item without being one"});
    if (policy != GrantPolicy::legitimate)
        return report({false, 0, "No inventory array the reward policy can place this in"});
    // Prefer the acquisition queue: the deferred pump publishes it as a real acquisition, which
    // is what plays the flyout. It takes only what it has proven commits, so the reward policy
    // still owns everything else, including a Dawning ingredient's balance and pickup row.
    if (server::bap::queue_item_acquisition(entry.identity.definitionIndex, quantity)) {
        Feedback queued{true};
        std::snprintf(queued.text.data(),
                      queued.text.size(),
                      "queued for acquisition; quantity=%d",
                      quantity);
        return queued;
    }
    const auto result = state::investment_edit::grant_item(
        entry.identity.definitionIndex, quantity, entry.identity.definitionHash);
    if (result.accepted) return report(result);
    Feedback output{};
    std::snprintf(output.text.data(), output.text.size(), "Unable to grant");
    return output;
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

/** An installed bounty: a character-bucket pursuit that expires and is not a reward marker. */
bool installed_bounty(std::uint16_t index,
                      data::items::Definition& item,
                      data::items::details::Definition& detail) noexcept {
    namespace bounty = state::runtime::detail::bounty;
    data::inventory::buckets::Descriptor bucket{};
    std::uint32_t markerHash = 0;
    // Zero lanes measure resolution only: an objective this build cannot read leaves the
    // granted bounty with nothing to progress, which is what the dummy pursuits look like.
    const std::array<std::int32_t, state::account::inventory::kItemObjectiveLaneCount> unset{};
    return bounty::reward_marker(index, markerHash) == bounty::RewardMarker::none
           && data::pursuits::measure(index, unset).resolved
           && data::find_item_definition_index(index, item)
           && data::find_configured_item_detail(index, detail)
           && detail.definitionHash == item.definitionHash && detail.bucketId == item.bucketId
           && detail.objectiveCount != 0 && detail.objectiveCount <= detail.objectiveIndices.size()
           && !detail.equipmentSlot.has_value() && detail.maxStackSize <= 1
           && detail.bucketId == data::items::kPursuitBucketId && detail.lifetimeSeconds > 0
           && data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
           && bucket.arraySelector == data::inventory::buckets::ArraySelector::character;
}

std::vector<std::uint16_t>& installed_bounty_order() noexcept {
    static std::vector<std::uint16_t> order;
    return order;
}

Pages bounty_pages() noexcept {
    // Installed definitions are fixed once the packages load, and this walks every one of them.
    // Caching the order on the definition count keeps both the count and the page off the frame.
    static Pages cached{};
    static std::size_t scanned{};
    const auto definitions = (std::min)(data::item_definition_count(), std::size_t{65536});
    if (definitions == scanned) return cached;
    auto& order = installed_bounty_order();
    try {
        order.clear();
        for (std::size_t i = 0; i < definitions; ++i) {
            data::items::Definition item{};
            data::items::details::Definition detail{};
            if (installed_bounty(static_cast<std::uint16_t>(i), item, detail))
                order.push_back(item.definitionIndex);
        }
    } catch (...) {
        order.clear();
        return {};
    }
    Pages pages{};
    pages.bounties = order.size();
    pages.count = (pages.bounties + kBountyPageSize - 1) / kBountyPageSize;
    cached = pages;
    scanned = definitions;
    return cached;
}

std::vector<std::uint16_t> bounty_page(std::size_t page) noexcept {
    const auto pages = bounty_pages();
    if (page == 0 || page > pages.count) return {};
    const auto& order = installed_bounty_order();
    const auto first = (page - 1) * kBountyPageSize;
    if (first >= order.size()) return {};
    const auto count = (std::min)(kBountyPageSize, order.size() - first);
    try {
        return std::vector<std::uint16_t>(order.begin() + static_cast<std::ptrdiff_t>(first),
                                          order.begin()
                                              + static_cast<std::ptrdiff_t>(first + count));
    } catch (...) {
        return {};
    }
}

bool queue_bounty_page(std::span<const std::uint16_t> indices) noexcept {
    return server::bap::queue_item_acquisitions(indices);
}

Feedback page_bounty(const Entry& entry) noexcept {
    if (!entry.bounty) return report({false, 0, "Not an installed bounty"});
    const auto grant = state::investment_edit::grant_item(
        entry.identity.definitionIndex, 1, entry.identity.definitionHash);
    if (!grant.accepted) return report(grant);
    const auto completion = state::investment_edit::complete_bounty(entry.identity.definitionIndex,
                                                                    entry.identity.definitionHash);
    return report({completion.accepted, grant.changed + completion.changed, completion.reason});
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
