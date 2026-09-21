#include "purchase_receipts.h"

#include <array>
#include <limits>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/runtime/eververse_receipts.h"
#include "../../../../state/runtime/runtime.h"
#include "abi.h"

namespace sunrise::middleware::datagen::family4::account {
namespace {

namespace eververse = state::eververse;
using Receipts = std::array<eververse::PurchaseReceipt, eververse::kRefundReceiptCapacity>;
static_assert(eververse::kRefundReceiptCapacity == layout::kPurchaseReceiptCapacity);

/** Resolve each tuple through the published runtime table and cross-check its reverse mapping. */
[[nodiscard]] bool resolve_item(std::uint32_t hash, std::uint16_t& index) noexcept {
    state::build_data::items::Definition byHash{}, byIndex{};
    if (hash == 0 || !state::build_data::find_item_definition_hash(hash, byHash)
        || byHash.definitionHash != hash
        || byHash.definitionIndex >= state::build_data::items::kDefinitionCapacity
        || !state::build_data::find_item_definition_index(byHash.definitionIndex, byIndex)
        || byIndex.definitionIndex != byHash.definitionIndex || byIndex.definitionHash != hash)
        return false;
    index = byHash.definitionIndex;
    return true;
}

[[nodiscard]] bool apply_mutation(const state::PendingRecordRewardGrant& mutation,
                                  Receipts& receipts) noexcept {
    const auto* addition = mutation.everversePurchase && mutation.everversePurchase->receipt
                               ? &*mutation.everversePurchase->receipt : nullptr;
    if (addition && mutation.everversePackage) return false;
    if (addition) {
        if (addition->sourceInstanceSoid == 0 || addition->slot >= receipts.size()
            || receipts[addition->slot].sourceInstanceSoid != 0)
            return false;
        receipts[addition->slot] = *addition;
    }
    if (mutation.everversePackage) {
        const auto& package = *mutation.everversePackage;
        const auto& before = package.receipt;
        if ((package.action != eververse::PackageAction::open
             && package.action != eververse::PackageAction::refund)
            || before.sourceInstanceSoid == 0 || before.slot >= receipts.size()
            || receipts[before.slot] != before)
            return false;
        receipts[before.slot] = {};
    }
    return true;
}

[[nodiscard]] bool encode_receipt(const eververse::PurchaseReceipt& receipt,
                                  layout::PurchaseReceipt& output) noexcept {
    if (receipt.characterSoid == 0 || receipt.purchasedAt <= 0
        || receipt.expiresAt <= receipt.purchasedAt || receipt.cost == 0
        || receipt.cost > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
        return false;
    std::uint16_t sourceIndex{}, currencyIndex{};
    if (!resolve_item(receipt.wrapperHash, sourceIndex)
        || !resolve_item(receipt.currencyHash, currencyIndex))
        return false;

    output.kind = 1;
    output.source.definitionIndex = sourceIndex;
    output.source.instanceSoid = receipt.sourceInstanceSoid;
    output.source.quantity = 1;
    output.returnItemCount = 1;
    output.returnItems[0].definitionIndex = currencyIndex;
    output.returnItems[0].quantity = static_cast<std::int32_t>(receipt.cost);
    output.expiresAt = receipt.expiresAt;
    output.characterSoid = receipt.characterSoid;
    return true;
}

} // namespace

void initialize_purchase_receipts(layout::Object& object) noexcept {
    object.purchaseReceipts = {};
}

bool project_purchase_receipts(std::uint64_t accountSoid,
                               layout::Object& object,
                               const state::PendingRecordRewardGrant* mutation) noexcept {
    if (accountSoid == 0 || object.accountSoid != accountSoid
        || (mutation && (!mutation->prepared || mutation->accountSoid != accountSoid)))
        return false;

    Receipts receipts{};
    if (!eververse::read_purchase_receipts(accountSoid, receipts)
        || (mutation && !apply_mutation(*mutation, receipts)))
        return false;

    layout::PurchaseReceiptBank bank{};
    for (std::size_t slot = 0; slot < receipts.size(); ++slot) {
        const auto& receipt = receipts[slot];
        if (receipt.sourceInstanceSoid == 0) continue;
        if (receipt.slot != slot) return false;
        for (std::size_t prior = 0; prior < slot; ++prior)
            if (receipts[prior].sourceInstanceSoid == receipt.sourceInstanceSoid) return false;
        if (!encode_receipt(receipt, bank.records[slot])) return false;
    }
    object.purchaseReceipts = bank;
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account
