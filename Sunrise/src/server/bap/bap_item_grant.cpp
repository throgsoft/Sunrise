#include <mutex>

#include "../../state/build_data/runtime.h"
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

} // namespace

bool grant_installed_item(std::uint16_t itemDefinitionIndex, std::int32_t quantity) noexcept {
    WorldRewardKind kind{};
    if (quantity < 1 || !reward_kind(itemDefinitionIndex, kind)) {
        return false;
    }
    // The queue helpers read and arm the peer table, so the lock is held across the whole grant.
    // Draining commits every queued reward without presentation, including one earned in world.
    const std::lock_guard lock(session_lock());
    bool queued = true;
    if (kind == WorldRewardKind::profileItem) {
        queued = arm_world_profile_item_acquisition(itemDefinitionIndex, quantity);
    } else {
        for (std::int32_t copy = 0; copy < quantity && queued; ++copy) {
            queued = arm_world_item_acquisition(itemDefinitionIndex);
        }
    }
    drain_world_rewards();
    arm_account_resync_everywhere();
    return queued;
}

} // namespace sunrise::server::bap
