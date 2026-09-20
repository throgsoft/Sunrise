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
#include "../../../state/runtime/investment_edit_runtime.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::client::ui::items::service {
namespace {
namespace data = state::build_data;

Feedback report(const state::investment_edit::Result& result) noexcept {
    // Every service has released SQLite before returning; publication uses the normal protocol.
    if (result.changed) {
        server::bap::request_account_resync();
    }
    // A refusal needs its reason; a success is already visible in the panel it changed.
    Feedback output{result.accepted};
    if (!result.accepted) {
        std::snprintf(output.text.data(), output.text.size(), "%s", result.reason);
    }
    return output;
}

} // namespace

Inventory inventory() noexcept {
    Inventory output{};
    try {
        const std::unique_ptr<state::AccountState> account(
            new state::AccountState(state::account_snapshot()));
        if (!state::account::valid(*account)) {
            return output;
        }
        for (std::size_t c = 0; c < (std::min)(account->characterCount, account->characters.size());
             ++c) {
            const auto& character = account->characters[c];
            if (!character.selected) {
                continue;
            }
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
                    || !detail.objectiveCount || detail.maxStackSize > 1) {
                    continue;
                }
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
                          if (left.complete != right.complete) {
                              return left.complete;
                          }
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
        || bucket.bucketId != identity.bucketId) {
        return GrantPolicy::unknown;
    }
    // A reward marker stands in for an item without ever being a resident of its own.
    if (state::is_bounty_reward_marker(identity.definitionIndex, identity.definitionHash)) {
        return GrantPolicy::dummy;
    }
    // A bucket that cannot transfer what it evicts is a delivery lane. It carries stack rows,
    // which the reward policy places and the bucket evicts in turn. An instanced row there is
    // not a resident at all: it stands for a side effect elsewhere, such as a weapon gaining a
    // plug, and minting it shows an item the account does not really own and never leaves.
    if ((bucket.policyFlags & buckets::kNoTransferOnEviction) != 0
        && detail.instancedDefinitionState
               != data::items::details::InstancedDefinitionState::stackable) {
        return GrantPolicy::unknown;
    }
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
    if (!output.accepted) {
        std::snprintf(output.text.data(), output.text.size(), "Unable to grant");
    }
    return output;
}

Feedback grant(const Entry& entry, std::int32_t quantity) noexcept {
    const auto policy = classify(entry);
    if (policy == GrantPolicy::dummy) {
        return report({false, 0, "Reward marker; it stands for an item without being one"});
    }
    if (policy != GrantPolicy::legitimate) {
        return report({false, 0, "No inventory array the reward policy can place this in"});
    }
    // Prefer the acquisition queue: the deferred pump publishes it as a real acquisition, which
    // is what plays the flyout. It takes only what it has proven commits, so the reward policy
    // still owns everything else, including a Dawning ingredient's balance and pickup row.
    if (server::bap::queue_item_acquisition(entry.identity.definitionIndex, quantity)) {
        return Feedback{true};
    }
    const auto result = state::investment_edit::grant_item(
        entry.identity.definitionIndex, quantity, entry.identity.definitionHash);
    if (result.accepted) {
        return report(result);
    }
    Feedback output{};
    std::snprintf(output.text.data(), output.text.size(), "Unable to grant");
    return output;
}
Feedback set_lane(std::uint64_t instance,
                  std::uint16_t item,
                  std::uint8_t lane,
                  std::int32_t value) noexcept {
    if (lane < state::account::inventory::kItemObjectiveLaneBase
        || lane >= state::account::inventory::kItemObjectiveCapacity) {
        return report({false, 0, "Choose objective lane 1..7"});
    }
    return report(state::investment_edit::set_objective_lane(instance, item, value, lane));
}
Feedback complete_bounties() noexcept {
    return report(state::investment_edit::complete_bounties());
}

/** An installed bounty: a character-bucket pursuit that expires and is not a reward marker. */
bool installed_bounty(std::uint16_t index,
                      data::items::Definition& item,
                      data::items::details::Definition& detail) noexcept {
    data::inventory::buckets::Descriptor bucket{};
    // Zero lanes measure the sources only. A pursuit whose objectives are shared or unsupported
    // cannot be finished by writing its own lanes, so the module has no way to exercise it.
    const std::array<std::int32_t, state::account::inventory::kItemObjectiveCapacity> unset{};
    const auto progress = data::pursuits::measure(index, unset);
    return progress.resolved && progress.itemBacked && data::find_item_definition_index(index, item)
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
    if (definitions == scanned) {
        return cached;
    }
    auto& order = installed_bounty_order();
    try {
        order.clear();
        for (std::size_t i = 0; i < definitions; ++i) {
            data::items::Definition item{};
            data::items::details::Definition detail{};
            if (installed_bounty(static_cast<std::uint16_t>(i), item, detail)) {
                order.push_back(item.definitionIndex);
            }
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
    if (page == 0 || page > pages.count) {
        return {};
    }
    const auto& order = installed_bounty_order();
    const auto first = (page - 1) * kBountyPageSize;
    if (first >= order.size()) {
        return {};
    }
    const auto count = (std::min)(kBountyPageSize, order.size() - first);
    try {
        return std::vector<std::uint16_t>(order.begin() + static_cast<std::ptrdiff_t>(first),
                                          order.begin()
                                              + static_cast<std::ptrdiff_t>(first + count));
    } catch (...) {
        return {};
    }
}

std::size_t grant_bounty_page(std::span<const std::uint16_t> indices, Feedback& refusal) noexcept {
    // Each grant commits separately; publish once to avoid a long acquisition presentation hold.
    refusal = {};
    std::size_t held = 0, changed = 0;
    for (const auto index : indices) {
        data::items::Definition item{};
        if (!data::find_item_definition_index(index, item)) {
            continue;
        }
        const auto result = state::investment_edit::grant_item(index, 1, item.definitionHash);
        changed += result.changed;
        if (result.accepted) {
            ++held;
        } else if (refusal.text[0] == '\0') {
            std::snprintf(refusal.text.data(), refusal.text.size(), "%s", result.reason);
        }
    }
    if (changed != 0) {
        server::bap::request_account_resync();
    }
    return held;
}

namespace {
std::vector<BucketItem> bucket_items(const state::AccountState& account, std::uint8_t bucketId) {
    std::vector<BucketItem> output;
    data::inventory::buckets::Descriptor bucket{};
    if (!data::find_inventory_bucket_descriptor(bucketId, bucket) || bucket.bucketId != bucketId) {
        return output;
    }
    if (!state::account::valid(account)) {
        return output;
    }
    const auto describe = [&](std::uint64_t instance,
                              std::uint32_t hash,
                              std::int32_t quantity,
                              std::int32_t serial,
                              bool resident,
                              state::account::inventory::ItemPlacement placement) {
        data::items::Definition identity{};
        data::items::details::Definition detail{};
        if (!data::find_item_definition_hash(hash, identity)
            || !(resident ? state::investment_edit::resident_in_bucket(
                                bucketId, identity.bucketId, placement)
                          : identity.bucketId == bucketId)
            || !data::find_configured_item_detail(identity.definitionIndex, detail)) {
            return;
        }
        BucketItem row{};
        row.instance = instance;
        row.hash = hash;
        row.index = identity.definitionIndex;
        row.quantity = quantity;
        row.maxStack = detail.maxStackSize;
        row.mutationSerial = serial;
        for (std::size_t c = 0; c < account.characterCount && !row.equipped; ++c) {
            for (const auto& slot : account.characters[c].equipment.slots) {
                if (slot && instance != 0 && slot->instanceSoid == instance) {
                    row.equipped = true;
                }
            }
        }
        output.push_back(row);
    };
    if (bucket.arraySelector == data::inventory::buckets::ArraySelector::profile) {
        for (std::size_t i = 0; i < account.profileItemCount; ++i) {
            const auto& item = account.profileItems[i];
            describe(item.instanceSoid,
                     item.definitionHash,
                     item.quantity,
                     item.mutationSerial,
                     false,
                     state::account::inventory::ItemPlacement::inventory);
        }
        return output;
    }
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        if (!character.selected) {
            continue;
        }
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            const auto& item = character.inventory.values[i];
            describe(item.instanceSoid,
                     item.definitionHash,
                     item.quantity,
                     item.mutationSerial,
                     true,
                     item.placement);
        }
        for (std::size_t i = 0; i < character.stacks.count; ++i) {
            const auto& row = character.stacks.values[i];
            describe(0,
                     row.definitionHash,
                     row.quantity,
                     row.mutationSerial,
                     false,
                     state::account::inventory::ItemPlacement::inventory);
        }
    }
    return output;
}
} // namespace

std::vector<BucketSummary> buckets() noexcept {
    std::vector<BucketSummary> output;
    try {
        const auto account = std::make_unique<state::AccountState>(state::account_snapshot());
        for (std::size_t id = 0; id <= 0xFFU; ++id) {
            data::inventory::buckets::Descriptor bucket{};
            const auto bucketId = static_cast<std::uint8_t>(id);
            if (!data::find_inventory_bucket_descriptor(bucketId, bucket)
                || bucket.bucketId != bucketId) {
                continue;
            }
            BucketSummary summary{};
            summary.bucketId = bucketId;
            summary.arraySelector = static_cast<std::uint8_t>(bucket.arraySelector);
            summary.firstSlot = bucket.firstSlot;
            summary.slotCount = bucket.slotCount;
            summary.policyFlags = bucket.policyFlags;
            summary.equipmentSlot = static_cast<std::int8_t>(bucket.equipmentSlot);
            summary.held = bucket_items(*account, bucketId).size();
            output.push_back(summary);
        }
    } catch (...) {
        return {};
    }
    return output;
}

std::vector<BucketItem> bucket_items(std::uint8_t bucketId) noexcept {
    try {
        const auto account = std::make_unique<state::AccountState>(state::account_snapshot());
        return bucket_items(*account, bucketId);
    } catch (...) {
        return {};
    }
}

Feedback clear_bucket(std::uint8_t bucketId) noexcept {
    return report(state::investment_edit::drop_bucket(bucketId));
}

Feedback
set_item_quantity(std::uint64_t instance, std::uint16_t index, std::int32_t quantity) noexcept {
    return report(state::investment_edit::set_held_quantity(instance, index, quantity));
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
