#pragma once

#include <cstddef>
#include <type_traits>

#include "layout.h"

namespace sunrise::middleware::datagen::family4::account::abi {

static_assert(sizeof(layout::PurchaseReceiptItem) == 24);
static_assert(offsetof(layout::PurchaseReceiptItem, definitionIndex) == 0);
static_assert(offsetof(layout::PurchaseReceiptItem, instanceSoid) == 8);
static_assert(offsetof(layout::PurchaseReceiptItem, quantity) == 16);
static_assert(sizeof(layout::PurchaseReceipt) == 216);
static_assert(offsetof(layout::PurchaseReceipt, returnItemCount) == 0);
static_assert(offsetof(layout::PurchaseReceipt, returnItems) == 8);
static_assert(offsetof(layout::PurchaseReceipt, kind) == 152);
static_assert(offsetof(layout::PurchaseReceipt, source) == 160);
static_assert(offsetof(layout::PurchaseReceipt, source)
                  + offsetof(layout::PurchaseReceiptItem, instanceSoid) == 168);
static_assert(offsetof(layout::PurchaseReceipt, source)
                  + offsetof(layout::PurchaseReceiptItem, quantity) == 176);
static_assert(offsetof(layout::PurchaseReceipt, secondarySelector) == 184);
static_assert(offsetof(layout::PurchaseReceipt, unknownValue) == 188);
static_assert(offsetof(layout::PurchaseReceipt, expiresAt) == 192);
static_assert(offsetof(layout::PurchaseReceipt, characterSoid) == 200);
static_assert(offsetof(layout::PurchaseReceipt, unknownEnum) == 208);
static_assert(sizeof(layout::PurchaseReceiptBank) == 6'488);
static_assert(offsetof(layout::PurchaseReceiptBank, unknownHeader) == 0);
static_assert(offsetof(layout::PurchaseReceiptBank, records) == 8);
static_assert(std::is_standard_layout_v<layout::PurchaseReceiptBank>);
static_assert(std::is_trivially_copyable_v<layout::PurchaseReceiptBank>);
static_assert(offsetof(layout::Object, purchaseReceipts) == layout::kPurchaseReceiptsOffset);
static_assert(offsetof(layout::Object, purchaseReceipts)
                  + offsetof(layout::PurchaseReceiptBank, records) == 88'248);
static_assert(offsetof(layout::Object, purchaseReceiptTailPadding) == 94'728);
static_assert(sizeof(layout::Object) == 96'280);

} // namespace sunrise::middleware::datagen::family4::account::abi
