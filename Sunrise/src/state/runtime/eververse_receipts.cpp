#include "eververse_receipts.h"

#include <limits>

#include "../investment/store_internal.h"
#include "eververse_runtime.h"

namespace sunrise::state::eververse {
namespace store = investment::store;
namespace {
bool valid_receipt(const PurchaseReceipt& receipt) noexcept {
    return receipt.sourceInstanceSoid >= account::inventory::kFirstProfileItemInstanceSoid
           && receipt.characterSoid != 0 && receipt.wrapperHash != 0 && receipt.itemHash != 0
           && receipt.currencyHash == kSilverHash && receipt.cost > 0
           && receipt.cost <= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
           && receipt.slot < kRefundReceiptCapacity && receipt.purchasedAt > 0
           && receipt.purchasedAt <= (std::numeric_limits<std::int64_t>::max)() - kRefundPeriodSeconds
           && receipt.expiresAt == receipt.purchasedAt + kRefundPeriodSeconds;
}
} // namespace

bool read_purchase_receipts(
    std::uint64_t accountSoid,
    std::array<PurchaseReceipt, kRefundReceiptCapacity>& receipts) noexcept {
    receipts = {};
    store::Transaction transaction;
    if (accountSoid == 0 || !transaction.ready()) return false;
    store::Statement rows("SELECT source_soid,character_soid,wrapper_hash,item_hash,currency_hash,"
                          "cost,vendor_index,sale_index,purchased_at,expires_at,slot "
                          "FROM eververse_receipts WHERE account_soid=? AND state=0 ORDER BY slot");
    if (!rows.parameters(accountSoid)) return false;
    int result{};
    while ((result = rows.step()) == SQLITE_ROW) {
        PurchaseReceipt receipt{};
        if (!rows.columns(receipt.sourceInstanceSoid, receipt.characterSoid, receipt.wrapperHash,
                          receipt.itemHash, receipt.currencyHash, receipt.cost, receipt.vendorIndex,
                          receipt.saleIndex, receipt.purchasedAt, receipt.expiresAt, receipt.slot)
            || !valid_receipt(receipt) || receipts[receipt.slot].sourceInstanceSoid != 0)
            return false;
        receipts[receipt.slot] = receipt;
    }
    return result == SQLITE_DONE && transaction.commit();
}

bool reserve_purchase_identity(std::uint64_t accountSoid, std::uint64_t minimum,
                                std::uint64_t& soid, std::uint8_t& slot) noexcept {
    soid = 0;
    slot = 0;
    if (minimum < account::inventory::kFirstProfileItemInstanceSoid
        || minimum > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        return false;
    store::Transaction transaction;
    std::array<PurchaseReceipt, kRefundReceiptCapacity> receipts{};
    if (!transaction.ready() || !read_purchase_receipts(accountSoid, receipts)) return false;
    std::size_t freeSlot{};
    while (freeSlot < receipts.size() && receipts[freeSlot].sourceInstanceSoid != 0) ++freeSlot;
    if (freeSlot == receipts.size()) return false;
    store::Statement row("SELECT COALESCE(MAX(source_soid),0) FROM eververse_receipts WHERE account_soid=?");
    std::uint64_t highest{};
    if (!row.parameters(accountSoid) || row.step() != SQLITE_ROW || !row.column(0, highest)
        || row.step() != SQLITE_DONE
        || highest >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        return false;
    soid = minimum > highest ? minimum : highest + 1;
    slot = static_cast<std::uint8_t>(freeSlot);
    return transaction.commit();
}

bool insert_purchase_receipt(std::uint64_t accountSoid, const PurchaseReceipt& receipt) noexcept {
    store::Transaction transaction;
    std::array<PurchaseReceipt, kRefundReceiptCapacity> current{};
    if (!transaction.ready() || !valid_receipt(receipt)
        || !read_purchase_receipts(accountSoid, current)
        || current[receipt.slot].sourceInstanceSoid != 0)
        return false;
    store::Statement row("INSERT INTO eververse_receipts "
                         "(account_soid,source_soid,character_soid,wrapper_hash,item_hash,currency_hash,"
                         "cost,vendor_index,sale_index,purchased_at,expires_at,slot,state) "
                         "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,0)");
    return row.write(accountSoid, receipt.sourceInstanceSoid, receipt.characterSoid,
                     receipt.wrapperHash, receipt.itemHash, receipt.currencyHash, receipt.cost,
                     receipt.vendorIndex, receipt.saleIndex, receipt.purchasedAt, receipt.expiresAt,
                     receipt.slot) && transaction.commit();
}

bool consume_purchase_receipt(std::uint64_t accountSoid, const PurchaseReceipt& before,
                               PackageAction action) noexcept {
    store::Transaction transaction;
    std::array<PurchaseReceipt, kRefundReceiptCapacity> receipts{};
    if (!transaction.ready() || !valid_receipt(before)
        || (action != PackageAction::open && action != PackageAction::refund)
        || !read_purchase_receipts(accountSoid, receipts) || receipts[before.slot] != before)
        return false;
    store::Statement row("UPDATE eververse_receipts SET state=? "
                         "WHERE account_soid=? AND source_soid=? AND state=0");
    return row.write(action == PackageAction::open ? 1 : 2, accountSoid, before.sourceInstanceSoid)
           && transaction.commit();
}
} // namespace sunrise::state::eververse
