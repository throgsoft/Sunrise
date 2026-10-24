#include <memory>
#include <new>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace family4_loadout = middleware::datagen::family4::loadout;
namespace inventory_buckets = build_data::inventory::buckets;
namespace item_details = build_data::items::details;
using namespace runtime::detail;

/** The first matching installed transfer rule controls a vendor claim. */
[[nodiscard]] static bool accepted_claim_rule(std::uint16_t vendorIndex,
                                              std::uint8_t sourceBucket) noexcept {
    std::uint8_t destination = 0;
    return sourceBucket == inventory_buckets::kLostItemsBucketId
           && build_data::find_vendor_transfer_destination(vendorIndex, sourceBucket, destination)
           && destination == build_data::vendors::kAuthoredDestination;
}

/** Stages one claim against the saved account without writing it. */
bool prepare_postmaster_claim(std::uint16_t vendorIndex,
                              std::uint8_t sourceBucket,
                              std::uint64_t instanceSoid,
                              std::uint16_t definitionIndex,
                              std::int32_t quantity,
                              PendingPostmasterClaim& mutation) noexcept {
    std::destroy_at(&mutation);
    std::construct_at(&mutation);
    const std::lock_guard lock(investment::store::g_mutex);
    const std::unique_ptr<AccountState> before(new (std::nothrow) AccountState{});
    const std::unique_ptr<AccountState> working(new (std::nothrow) AccountState{});
    if (!before || !working || !accepted_claim_rule(vendorIndex, sourceBucket)) {
        return false;
    }
    *before = account_snapshot();
    const std::size_t characterIndex = selected_character_index(*before);
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (!account::valid(*before) || !valid_profile_inventory(*before)
        || characterIndex >= before->characterCount
        || !build_data::find_item_definition_index(definitionIndex, item)
        || !build_data::find_configured_item_detail(definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex) {
        return false;
    }
    *working = *before;
    PlacedRows placed;
    const char* reason = nullptr;
    family4_loadout::ResolvedLoadout loadout{};
    if (!place_claim(*working, characterIndex, item, detail, instanceSoid, quantity, placed, reason)
        || !account::valid(*working) || !valid_profile_inventory(*working)
        || !family4_loadout::resolve(*working, characterIndex, loadout)
        || !record_grant_images(
            *before, *working, characterIndex, placed, mutation.grant, reason)) {
        mutation = {};
        return false;
    }
    mutation.grant.vendorTransfer = true;
    mutation.instanceSoid = instanceSoid;
    mutation.quantity = quantity;
    mutation.vendorIndex = vendorIndex;
    mutation.definitionIndex = definitionIndex;
    mutation.sourceBucket = sourceBucket;
    mutation.prepared = true;
    return true;
}

/** Rebuilds the claimed account for publication without writing it. */
bool preview_postmaster_claim(const PendingPostmasterClaim& mutation,
                              AccountState& after) noexcept {
    return mutation.prepared && accepted_claim_rule(mutation.vendorIndex, mutation.sourceBucket)
           && preview_record_reward_grant(mutation.grant, after);
}

/** Writes the claim after re-checking its vendor rule and saved before-image in one transaction. */
bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept {
    const PendingConsumption consume{mutation};
    return mutation.prepared && mutation.grant.vendorTransfer
           && accepted_claim_rule(mutation.vendorIndex, mutation.sourceBucket)
           && commit_record_reward(mutation.grant);
}

} // namespace sunrise::state
