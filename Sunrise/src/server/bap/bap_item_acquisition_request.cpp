#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../../state/build_data/items/quest_initialization.h"
#include "../../state/build_data/runtime.h"
#include "../../state/investment/store_internal.h"
#include "../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** Reads which world reward queue carries this item from the bucket its definition names. */
[[nodiscard]] bool reward_kind(std::uint16_t itemDefinitionIndex,
                               WorldRewardKind& kind,
                               std::uint32_t& definitionHash,
                               bool& uniquePursuit) noexcept {
    namespace data = state::build_data;
    data::items::Definition item{};
    data::items::details::Definition detail{};
    data::inventory::buckets::Descriptor bucket{};
    if (!data::find_item_definition_index(itemDefinitionIndex, item)
        || !data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || !data::find_inventory_bucket_descriptor(item.bucketId, bucket)
        || bucket.bucketId != item.bucketId) {
        return false;
    }
    definitionHash = item.definitionHash;
    if (bucket.arraySelector != data::inventory::buckets::ArraySelector::character) {
        kind = WorldRewardKind::profileItem;
        return true;
    }
    // A stack row in a character bucket is placed by the reward policy, not by the instanced
    // acquisition the ordinary item kind commits with.
    kind =
        detail.instancedDefinitionState == data::items::details::InstancedDefinitionState::stackable
            ? WorldRewardKind::characterStack
            : WorldRewardKind::item;
    uniquePursuit = kind == WorldRewardKind::item
                    && detail.bucketId == data::items::kPursuitBucketId
                    && !detail.equipmentSlot.has_value() && detail.maxStackSize <= 1;
    return true;
}

/**
 * Proves the reward commits before the queue saves it.
 * A queued reward whose commit fails is retained for a later attempt, and the oldest is always
 * read first, so one that can never commit would hold every reward behind it.
 *
 * An instanced reward is proven twice. The reward preparer threads one working account image
 * through every copy, so an item the account may hold only once refuses its second copy here
 * rather than in the queue. The pump commits through a different preparer, so that one is asked
 * as well: a definition only the reward path can place, such as a stack row in a delivery lane,
 * is refused here and granted through the reward path instead of jamming the queue.
 */
[[nodiscard]] bool
commits(std::uint16_t itemDefinitionIndex, std::int32_t quantity, WorldRewardKind kind) noexcept {
    if (kind == WorldRewardKind::profileItem) {
        const std::unique_ptr<state::PendingProfileItemAcquisition> probe(
            new (std::nothrow) state::PendingProfileItemAcquisition);
        return probe
               && state::prepare_profile_item_acquisition_for_item(
                   itemDefinitionIndex, quantity, *probe);
    }
    if (quantity > static_cast<std::int32_t>(state::kRecordRewardGrantCapacity)) {
        return false;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> probe(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!probe) {
        return false;
    }
    if (kind == WorldRewardKind::characterStack) {
        const std::array rows{state::DirectRecordReward{itemDefinitionIndex, quantity}};
        return state::prepare_record_reward_grant(rows, state::kUnclaimedRecordIndex, *probe);
    }
    const auto count = static_cast<std::size_t>(quantity);
    std::array<state::DirectRecordReward, state::kRecordRewardGrantCapacity> rows{};
    std::fill_n(rows.begin(), count, state::DirectRecordReward{itemDefinitionIndex, 1});
    if (!state::prepare_record_reward_grant(
            std::span(rows).first(count), state::kUnclaimedRecordIndex, *probe)) {
        return false;
    }
    const std::unique_ptr<state::PendingItemAcquisition> pumped(new (std::nothrow)
                                                                    state::PendingItemAcquisition);
    return pumped && state::prepare_item_acquisition_for_item(itemDefinitionIndex, *pumped);
}

} // namespace

bool queue_item_acquisition(std::uint16_t itemDefinitionIndex, std::int32_t quantity) noexcept {
    WorldRewardKind kind{};
    std::uint32_t definitionHash{};
    bool uniquePursuit{};
    const char* refused = quantity < 1 ? "quantity"
                          : !reward_kind(itemDefinitionIndex, kind, definitionHash, uniquePursuit)
                              ? "bucket"
                              : nullptr;
    if (refused != nullptr) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=item_acquisition stage=queue result=fail reason=%s item=%u "
                          "quantity=%d",
                          refused,
                          static_cast<unsigned>(itemDefinitionIndex),
                          quantity);
        return false;
    }
    // Hold both locks across the account preflight and queue insert. A pending unique pursuit
    // then reserves its identity until the pump commits or retires it.
    const std::lock_guard lock(session_lock());
    const auto copies = kind == WorldRewardKind::item ? static_cast<std::size_t>(quantity) : 1U;
    const auto rowQuantity = kind == WorldRewardKind::item ? 1 : quantity;
    bool alreadyPending = false;
    {
        state::investment::store::Transaction transaction;
        const char* preflightRefusal = !transaction.ready()                            ? "database"
                                       : !commits(itemDefinitionIndex, quantity, kind) ? "policy"
                                                                                       : nullptr;
        if (preflightRefusal != nullptr) {
            core::log::writef(core::log::Channel::server,
                              core::log::Level::warn,
                              "ev=item_acquisition stage=queue result=fail reason=%s item=%u "
                              "quantity=%d",
                              preflightRefusal,
                              static_cast<unsigned>(itemDefinitionIndex),
                              quantity);
            return false;
        }
        if (!state::investment::store::enqueue_reward_copies(definitionHash,
                                                             rowQuantity,
                                                             static_cast<std::uint8_t>(kind),
                                                             copies,
                                                             uniquePursuit,
                                                             alreadyPending)
            || !transaction.commit()) {
            return false;
        }
    }
    // The first request still owns this pending grant; a direct fallback would duplicate it.
    if (alreadyPending) {
        if (!has_active_family4_peer()) {
            settle_world_reward();
        }
        return true;
    }
    // With no Family-4 peer, the existing queue policy settles one oldest reward per copy.
    if (!has_active_family4_peer()) {
        for (std::size_t copy = 0; copy < copies; ++copy) {
            settle_world_reward();
        }
    }
    return true;
}

} // namespace sunrise::server::bap
