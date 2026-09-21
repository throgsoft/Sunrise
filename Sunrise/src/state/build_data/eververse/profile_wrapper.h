#pragma once

#include "../runtime.h"
#include "manifest_catalog.h"

namespace sunrise::state::build_data::eververse {

/**
 * Resolve a wrapper's stackable contents against installed details and local manifest metadata.
 * Only the ornament/shader bucket runs already reserved for profile residents are supported.
 * The purchase transaction separately proves that this pair belongs to its vendor sale.
 */
[[nodiscard]] inline bool resolve_profile_wrapper(
    const items::Definition& wrapper,
    const items::details::Definition& wrapperDetail,
    std::uint32_t containedHash,
    std::uint16_t& containedIndex) noexcept {
    containedIndex = items::details::kUnavailableItemIndex;
    if (containedHash == 0 || containedHash == wrapper.definitionHash
        || (wrapper.bucketId != 13 && wrapper.bucketId != 14)
        || wrapperDetail.definitionIndex != wrapper.definitionIndex
        || wrapperDetail.definitionHash != wrapper.definitionHash
        || wrapperDetail.bucketId != wrapper.bucketId || wrapperDetail.maxStackSize != 1
        || wrapperDetail.instancedDefinitionState
               != items::details::InstancedDefinitionState::instanced
        || wrapperDetail.equipmentSlot || wrapperDetail.objectiveCount != 0
        || wrapperDetail.rewardCount != 0 || wrapperDetail.ordinarySocketCount != 0
        || wrapperDetail.ordinarySocketState != items::details::OrdinarySocketState::absent)
        return false;

    inventory::buckets::Descriptor bucket{};
    ItemMetadata wrapperMetadata{}, contentsMetadata{};
    items::Definition contents{};
    items::details::Definition contentsDetail{};
    socket_entry_lists::Definition socketList{};
    if (!find_inventory_bucket_descriptor(wrapper.bucketId, bucket)
        || bucket.bucketId != wrapper.bucketId
        || bucket.arraySelector != inventory::buckets::ArraySelector::profile
        || bucket.slotCount == 0 || bucket.slotCount > 50
        || !find_socket_entry_list(wrapperDetail.socketEntryListIndex, socketList)
        || socketList.definitionIndex != wrapperDetail.socketEntryListIndex
        || socketList.entryCount != 0
        || !read_item(wrapper.definitionHash, wrapperMetadata)
        || wrapperMetadata.itemHash != wrapper.definitionHash
        || wrapperMetadata.itemIndex != wrapper.definitionIndex
        || wrapperMetadata.bucketIndex != wrapper.bucketId || wrapperMetadata.maxStackSize != 1
        || !wrapperMetadata.isWrapper || !wrapperMetadata.instanced || wrapperMetadata.isDummy
        || wrapperMetadata.previewVendorHash != 0 || wrapperMetadata.useOnAcquire
        || wrapperMetadata.onActionRecreateSelf || wrapperMetadata.unlockAction
        || !is_simple_wrapper_action(wrapper.definitionHash)
        || !find_item_definition_hash(containedHash, contents)
        || contents.definitionHash != containedHash || contents.bucketId != wrapper.bucketId
        || !find_configured_item_detail(contents.definitionIndex, contentsDetail)
        || contentsDetail.definitionIndex != contents.definitionIndex
        || contentsDetail.definitionHash != containedHash
        || contentsDetail.bucketId != contents.bucketId || contentsDetail.maxStackSize <= 0
        || contentsDetail.instancedDefinitionState
               != items::details::InstancedDefinitionState::stackable
        || contentsDetail.equipmentSlot
        || !read_item(containedHash, contentsMetadata)
        || contentsMetadata.itemHash != containedHash
        || contentsMetadata.itemIndex != contents.definitionIndex
        || contentsMetadata.bucketIndex != contents.bucketId
        || contentsMetadata.maxStackSize != contentsDetail.maxStackSize
        || contentsMetadata.instanced || contentsMetadata.isWrapper || contentsMetadata.isDummy
        || contentsMetadata.previewVendorHash != 0 || contentsMetadata.useOnAcquire
        || contentsMetadata.onActionRecreateSelf)
        return false;
    containedIndex = contents.definitionIndex;
    return true;
}

} // namespace sunrise::state::build_data::eververse
