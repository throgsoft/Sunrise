#include <memory>
#include <mutex>
#include <new>

#include "../../state/build_data/runtime.h"
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
 * The prepare judges one copy against the committed account, so a request for several copies
 * proves only the first. The difference the rest can make is capacity, and a reward the queue
 * holds for want of room commits by itself once room exists. A definition the acquisition path
 * refuses outright never clears, and that is what this has to catch.
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
    const std::unique_ptr<state::PendingItemAcquisition> probe(new (std::nothrow)
                                                                   state::PendingItemAcquisition);
    return probe && state::prepare_item_acquisition_for_item(itemDefinitionIndex, *probe);
}

} // namespace

bool queue_item_acquisition(std::uint16_t itemDefinitionIndex, std::int32_t quantity) noexcept {
    WorldRewardKind kind{};
    if (quantity < 1 || !reward_kind(itemDefinitionIndex, kind)
        || !commits(itemDefinitionIndex, quantity, kind)) {
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
