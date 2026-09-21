#include "codec.h"

namespace sunrise::state::build_data::cache::records {

/** Encodes one vendor index row. */
bool encode(const vendors::IndexEntry& value, VendorIndexRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.definitionTag = value.definitionTag;
    record.index = value.index;
    return true;
}

/** Decodes one vendor index row. */
bool decode(const VendorIndexRecord& record, vendors::IndexEntry& value) noexcept {
    value = {};
    if (record.reserved != 0) {
        return false;
    }
    value = {record.definitionHash, record.definitionTag, record.index};
    return true;
}

/** Encodes one vendor definition and its flat-bank ranges. */
bool encode(const vendors::Definition& value, VendorDefinitionRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.definitionTag = value.definitionTag;
    record.definitionClass = value.definitionClass;
    record.definitionSize = value.definitionSize;
    record.installedRowBase = value.installedRowBase;
    record.installedRowClass = value.installedRowClass;
    record.saleRowBase = value.saleRowBase;
    record.saleRowClass = value.saleRowClass;
    record.thirdRowBase = value.thirdRowBase;
    record.thirdRowClass = value.thirdRowClass;
    record.saleRowOffset = value.saleRowOffset;
    record.installedRowOffset = value.installedRowOffset;
    record.resetIntervalRaw = value.resetIntervalRaw;
    record.resetPhaseRaw = value.resetPhaseRaw;
    record.index = value.index;
    record.installedCount = value.installedCount;
    record.saleCount = value.saleCount;
    record.thirdCount = value.thirdCount;
    record.transferRulesAvailable = value.transferRulesAvailable ? 1 : 0;
    record.transferRuleCount = value.transferRuleCount;
    for (std::size_t row = 0; row < value.transferRules.size(); ++row) {
        record.transferRules[row * 2] = value.transferRules[row].sourceBucket;
        record.transferRules[row * 2 + 1] = value.transferRules[row].destinationBucket;
    }
    return true;
}

/** Decodes one vendor definition and its flat-bank ranges. */
bool decode(const VendorDefinitionRecord& record, vendors::Definition& value) noexcept {
    value = {};
    // The catalog checks every range against the whole domain. Only the class is checked here.
    // A row of another class is not a vendor definition, whatever its ranges say.
    if (record.definitionClass != vendors::kDefinitionClass || record.transferRulesAvailable > 1
        || record.transferRuleCount > vendors::kTransferRuleCapacity) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.definitionTag = record.definitionTag;
    value.definitionClass = record.definitionClass;
    value.definitionSize = record.definitionSize;
    value.installedRowBase = record.installedRowBase;
    value.installedRowClass = record.installedRowClass;
    value.saleRowBase = record.saleRowBase;
    value.saleRowClass = record.saleRowClass;
    value.thirdRowBase = record.thirdRowBase;
    value.thirdRowClass = record.thirdRowClass;
    value.saleRowOffset = record.saleRowOffset;
    value.installedRowOffset = record.installedRowOffset;
    value.resetIntervalRaw = record.resetIntervalRaw;
    value.resetPhaseRaw = record.resetPhaseRaw;
    value.index = record.index;
    value.installedCount = record.installedCount;
    value.saleCount = record.saleCount;
    value.thirdCount = record.thirdCount;
    value.transferRulesAvailable = record.transferRulesAvailable != 0;
    value.transferRuleCount = record.transferRuleCount;
    for (std::size_t row = 0; row < value.transferRules.size(); ++row) {
        value.transferRules[row].sourceBucket = record.transferRules[row * 2];
        value.transferRules[row].destinationBucket = record.transferRules[row * 2 + 1];
    }
    return true;
}

/** Encodes one vendor sale row. */
bool encode(const vendors::SaleRow& value, VendorSaleRowRecord& record) noexcept {
    record = {};
    if (!vendors::valid_refund_policy(value.refundPolicy)) {
        return false;
    }
    record.itemIndex = value.itemIndex;
    record.secondaryItemIndex = value.secondaryItemIndex;
    record.categoryIndex = value.categoryIndex;
    record.costQuantity = value.costQuantity;
    record.costItemIndex = value.costItemIndex;
    record.quantity = value.quantity;
    record.costCount = value.costCount;
    record.purchaseUnlockSlot = value.purchaseUnlockSlot;
    record.costIsConstant = value.costIsConstant ? 1 : 0;
    record.purchaseGate = static_cast<std::uint8_t>(value.purchaseGate);
    record.refundPolicy = static_cast<std::uint8_t>(value.refundPolicy);
    return true;
}

/** Decodes one vendor sale row. */
bool decode(const VendorSaleRowRecord& record, vendors::SaleRow& value) noexcept {
    value = {};
    if (record.reserved != decltype(record.reserved){} || record.reservedStore != 0
        || record.costIsConstant > 1
        || !vendors::valid_refund_policy(static_cast<vendors::RefundPolicy>(record.refundPolicy))
        || record.purchaseGate > static_cast<std::uint8_t>(vendors::PurchaseGate::notOwned)) {
        return false;
    }
    value.itemIndex = record.itemIndex;
    value.secondaryItemIndex = record.secondaryItemIndex;
    value.categoryIndex = record.categoryIndex;
    value.costQuantity = record.costQuantity;
    value.costItemIndex = record.costItemIndex;
    value.quantity = record.quantity;
    value.costCount = record.costCount;
    value.purchaseUnlockSlot = record.purchaseUnlockSlot;
    value.costIsConstant = record.costIsConstant != 0;
    value.purchaseGate = static_cast<vendors::PurchaseGate>(record.purchaseGate);
    value.refundPolicy = static_cast<vendors::RefundPolicy>(record.refundPolicy);
    return true;
}

/** Encodes one vendor category row. */
bool encode(const vendors::InstalledRow& value, VendorInstalledRowRecord& record) noexcept {
    record = {value.definitionHash};
    return true;
}

/** Decodes one vendor category row. */
bool decode(const VendorInstalledRowRecord& record, vendors::InstalledRow& value) noexcept {
    value = {record.definitionHash};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
