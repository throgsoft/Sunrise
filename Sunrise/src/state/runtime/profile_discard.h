#pragma once

#include <algorithm>
#include <limits>

#include "item_discard_policy.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::item_discard {

/** Resolves the local material policy against the currently installed native catalog. */
[[nodiscard]] inline const Policy*
installed(std::uint32_t hash, build_data::items::details::Definition& detail) noexcept {
    const auto* policy = find(hash);
    build_data::items::Definition item{};
    detail = {};
    build_data::inventory::buckets::Descriptor bucket{};
    if (!policy || !build_data::find_item_definition_hash(hash, item) || item.definitionHash != hash
        || !build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionHash != hash || detail.definitionIndex != item.definitionIndex
        || detail.bucketId != item.bucketId || detail.maxStackSize <= 0
        || detail.equipmentSlot.has_value()
        || detail.instancedDefinitionState
               != build_data::items::details::InstancedDefinitionState::stackable
        || build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
        || bucket.bucketId != item.bucketId
        || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile)
        return nullptr;
    return policy;
}

/** Recomputes the profile-only transition, including when no account settings were captured. */
[[nodiscard]] inline bool stage_profile(const AccountState& before,
                                        std::uint32_t hash,
                                        std::int32_t observed,
                                        PendingProfileItemAcquisition& out) noexcept {
    out = {};
    build_data::items::details::Definition detail{};
    const auto* policy = installed(hash, detail);
    if (!policy || before.primarySoid == 0 || !runtime::detail::valid_profile_inventory(before))
        return false;
    const auto consumed = quantity(*policy, observed, detail.maxStackSize);
    if (consumed == 0) return false;
    auto target = before.profileItems.size();
    std::int32_t serial = 0;
    for (std::size_t i = 0; i < before.profileItemCount; ++i) {
        const auto& row = before.profileItems[i];
        serial = (std::max)(serial, row.mutationSerial);
        if (row.definitionHash != hash) continue;
        if (target != before.profileItems.size() || row.instanceSoid || row.quantity != observed)
            return false;
        target = i;
    }
    if (target == before.profileItems.size()) return false;
    const auto remaining = observed - consumed;
    if (remaining && serial == (std::numeric_limits<std::int32_t>::max)()) return false;
    out.beforeItems = before.profileItems;
    out.afterItems = before.profileItems;
    out.accountSoid = before.primarySoid;
    out.acquiredDefinitionHash = hash;
    out.expectedItemCount = before.profileItemCount;
    out.afterItemCount = before.profileItemCount;
    out.profileIndex = target;
    out.previousQuantity = observed;
    out.acquiredQuantity = remaining;
    out.previousMutationSerial = before.profileItems[target].mutationSerial;
    out.bucketId = detail.bucketId;
    if (remaining) {
        out.acquiredMutationSerial = serial + 1;
        out.afterItems[target].quantity = remaining;
        out.afterItems[target].mutationSerial = serial + 1;
    } else {
        for (std::size_t i = target + 1; i < out.afterItemCount; ++i)
            out.afterItems[i - 1] = out.afterItems[i];
        out.afterItems[--out.afterItemCount] = {};
    }
    out.profileDiscard = true;
    out.prepared = true;
    return true;
}

/** A no-SOID request must identify exactly one owned stack in a valid, loaded account. */
[[nodiscard]] inline bool stage(const AccountState& before,
                                std::uint32_t hash,
                                std::int32_t observed,
                                PendingProfileItemAcquisition& out) noexcept {
    out = {};
    return account::valid(before) && stage_profile(before, hash, observed, out);
}

/** Recompute the entire deletion rather than accepting a caller-authored afterimage. */
[[nodiscard]] inline bool canonical(const PendingProfileItemAcquisition& mutation) noexcept {
    if (!mutation.profileDiscard || !mutation.prepared || mutation.appended || mutation.actionSource
        || mutation.acquiredInstanceSoid || mutation.directGrant
        || mutation.materialRequirementSetHash || mutation.materialRequirementCount
        || mutation.collectibleIndex || mutation.changeCount)
        return false;
    for (const auto& change : mutation.changes)
        if (change.mutationSerial || change.afterQuantity) return false;
    AccountState before{};
    before.primarySoid = mutation.accountSoid;
    before.profileItems = mutation.beforeItems;
    before.profileItemCount = mutation.expectedItemCount;
    PendingProfileItemAcquisition expected{};
    // This view deliberately contains no settings. The public stage and materialize boundaries
    // validate the full loaded account; canonical validation owns only the captured profile.
    if (!stage_profile(
            before, mutation.acquiredDefinitionHash, mutation.previousQuantity, expected))
        return false;
    return mutation.profileIndex == expected.profileIndex && mutation.bucketId == expected.bucketId
           && mutation.acquiredQuantity == expected.acquiredQuantity
           && mutation.previousMutationSerial == expected.previousMutationSerial
           && mutation.acquiredMutationSerial == expected.acquiredMutationSerial
           && runtime::detail::same_profile_views(mutation.afterItems,
                                                  mutation.afterItemCount,
                                                  expected.afterItems,
                                                  expected.afterItemCount);
}

/** Apply only against the complete captured profile, preserving unrelated current state. */
[[nodiscard]] inline bool materialize(const AccountState& current,
                                      const PendingProfileItemAcquisition& mutation,
                                      AccountState& after) noexcept {
    if (!canonical(mutation) || current.primarySoid != mutation.accountSoid
        || !account::valid(current) || !runtime::detail::valid_profile_inventory(current)
        || !runtime::detail::same_profile_inventory(
            current, mutation.beforeItems, mutation.expectedItemCount))
        return false;
    after = current;
    after.profileItems = mutation.afterItems;
    after.profileItemCount = mutation.afterItemCount;
    return account::valid(after) && runtime::detail::valid_profile_inventory(after);
}

} // namespace sunrise::state::item_discard
