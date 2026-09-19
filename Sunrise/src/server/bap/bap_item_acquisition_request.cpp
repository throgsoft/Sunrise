#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../../state/build_data/runtime.h"
#include "../../state/investment/store_internal.h"
#include "../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** Reads which world reward queue carries this item from the bucket its definition names. */
[[nodiscard]] bool reward_kind(std::uint16_t itemDefinitionIndex, WorldRewardKind& kind) noexcept {
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
    kind = bucket.arraySelector == data::inventory::buckets::ArraySelector::character
               ? WorldRewardKind::item
               : WorldRewardKind::profileItem;
    return true;
}

/**
 * Proves the reward commits before the queue saves it.
 * A queued reward whose commit fails is retained for a later attempt, and the oldest is always
 * read first, so one that can never commit would hold every reward behind it. This prepares
 * through the same function the deferred pump commits with, and discards the result.
 *
 * Several copies are proven together, not one at a time. The reward preparer threads one
 * working account image through every row, so it sees what the earlier copies did: an item the
 * account may hold only once refuses its second copy here rather than in the queue, where it
 * would never commit and would hold every later reward behind it.
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
    if (quantity > static_cast<std::int32_t>(state::kRecordRewardGrantCapacity)) return false;
    const std::unique_ptr<state::PendingRecordRewardGrant> probe(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!probe) return false;
    const auto count = static_cast<std::size_t>(quantity);
    std::array<state::DirectRecordReward, state::kRecordRewardGrantCapacity> rows{};
    std::fill_n(rows.begin(), count, state::DirectRecordReward{itemDefinitionIndex, 1});
    return state::prepare_record_reward_grant(
        std::span(rows).first(count), state::kUnclaimedRecordIndex, *probe);
}

/**
 * Proves a whole page against one working account image, in preparer-sized runs.
 * One run threads every row through a single image, so a copy the account may not hold twice is
 * refused here rather than in the queue, where it would never commit.
 */
[[nodiscard]] bool commits_together(std::span<const std::uint16_t> itemDefinitionIndices) noexcept {
    const std::unique_ptr<state::PendingRecordRewardGrant> probe(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!probe) return false;
    std::array<state::DirectRecordReward, state::kRecordRewardGrantCapacity> rows{};
    for (std::size_t first = 0; first < itemDefinitionIndices.size();
         first += state::kRecordRewardGrantCapacity) {
        const auto count =
            (std::min)(state::kRecordRewardGrantCapacity, itemDefinitionIndices.size() - first);
        for (std::size_t row = 0; row < count; ++row) {
            rows[row] = state::DirectRecordReward{itemDefinitionIndices[first + row], 1};
        }
        if (!state::prepare_record_reward_grant(
                std::span(rows).first(count), state::kUnclaimedRecordIndex, *probe)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool queue_item_acquisitions(std::span<const std::uint16_t> itemDefinitionIndices) noexcept {
    if (itemDefinitionIndices.empty() || !commits_together(itemDefinitionIndices)) return false;
    // Saving each reward on its own durably commits once per item. One transaction spends a
    // single commit on the whole page while the pump still publishes every acquisition.
    const std::lock_guard lock(session_lock());
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) return false;
    for (const auto index : itemDefinitionIndices) {
        WorldRewardKind kind{};
        if (!reward_kind(index, kind)) return false;
        const bool armed = kind == WorldRewardKind::profileItem
                               ? arm_world_profile_item_acquisition(index, 1)
                               : arm_world_item_acquisition(index);
        if (!armed) return false;
    }
    return transaction.commit();
}

bool queue_item_acquisition(std::uint16_t itemDefinitionIndex, std::int32_t quantity) noexcept {
    WorldRewardKind kind{};
    const char* refused = quantity < 1                                    ? "quantity"
                          : !reward_kind(itemDefinitionIndex, kind)       ? "bucket"
                          : !commits(itemDefinitionIndex, quantity, kind) ? "policy"
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
    // Arming reads the peer table and may settle the reward, so the session lock covers both.
    const std::lock_guard lock(session_lock());
    if (kind == WorldRewardKind::profileItem)
        return arm_world_profile_item_acquisition(itemDefinitionIndex, quantity);
    // One instanced copy per queued reward, so each arrives with its own acquisition.
    for (std::int32_t copy = 0; copy < quantity; ++copy)
        if (!arm_world_item_acquisition(itemDefinitionIndex)) return false;
    return true;
}

} // namespace sunrise::server::bap
