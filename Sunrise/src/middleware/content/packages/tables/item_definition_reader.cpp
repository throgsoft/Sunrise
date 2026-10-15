#include <algorithm>

#include "../../../../state/build_data/items/quest_initialization.h"
#include "definition_index_table.h"
#include "internal.h"
#include "items.h"

namespace sunrise::middleware::content::packages::tables::items {
namespace {

/** The signed stack limit comes before the bucket id. */
constexpr std::size_t kMaxStackSizeOffset = 180;
/** A nonzero predicate byte marks an instanced definition. */
constexpr std::size_t kInstancedOffset = 187;
/**
 * Definition byte 186 is the item tier: 1 common, 2 uncommon, 3 rare, 4 legendary, 5 exotic, and
 * 0 for rows outside the rarity ladder such as currencies. Confirmed against every installed
 * legendary weapon and armor row and the currency rows.
 */
constexpr std::size_t kTierOffset = 186;
/** The manifest's stack-unique label, as the FNV-1 hash of its string. */
constexpr std::size_t kStackUniqueLabelOffset = 176;
/**
 * Definition byte 185 is the manifest's recovery bucket as a runtime bucket id, or all bits set
 * when the item has none.
 */
constexpr std::size_t kRecoveryBucketOffset = 185;
/**
 * Bytes 189 and 190 hold 0 or 1. The client's change-record writer turns them into the policy
 * bits that skip the acquisition toast and count, or only the toast.
 */
constexpr std::size_t kQuietAcquisitionOffset = 189;
constexpr std::size_t kToastlessAcquisitionOffset = 190;
/** The equipment block is self-relative from this offset, zero when absent. */
constexpr std::size_t kEquipmentBlockOffset = 16;
/** The equipment block stores its signed slot id here. */
constexpr std::size_t kEquipmentSlotOffset = 24;
/** The socket entry list block is self-relative from this offset, zero when absent. */
constexpr std::size_t kSocketEntryListBlockOffset = 128;
/** A present block declares 12 bytes. A shorter tail is not a block. */
constexpr std::size_t kSocketEntryListBlockSize = 12;
/** Equipment slots run from 0 through 19. */
constexpr std::int32_t kEquipmentSlotCount = 20;
/** A socket entry stores its type index first and its initial plug next. */
constexpr std::size_t kSocketTypeOffset = 0;
constexpr std::size_t kSocketPlugOffset = 2;
/** Fixed fields end after the acquisition presentation bytes; every installed item reaches it. */
constexpr std::size_t kFixedFieldEnd = kToastlessAcquisitionOffset + 1;
/** Optional plug category used to expand three native reusable plug families. */
constexpr std::size_t kPlugCategoryOffset = 392;
/**
 * The plug block is an embedded record whose class marker precedes its fields. It usually starts
 * at byte 388, so the category sits at kPlugCategoryOffset, but an optional record ahead of it
 * moves it, so the block is located by its marker inside this window rather than assumed.
 */
constexpr std::uint32_t kPlugBlockClass = 0x808077E3U;
constexpr std::size_t kPlugBlockSearchStart = 0x100;
constexpr std::size_t kPlugBlockSearchEnd = 0x300;
/** Category hash and server-roll set ordinal, relative to the plug block marker. */
constexpr std::size_t kPlugBlockCategoryOffset = 4;
constexpr std::size_t kPlugBlockRollSetOffset = 0x26;
/**
 * A plug that grants another plug's effect (a Year-1 armor masterwork result naming the stat
 * perk it stands for) carries one linked-plug record; its item index sits at byte 12.
 */
constexpr std::uint32_t kLinkedPlugClass = 0x80803036U;
constexpr std::size_t kLinkedPlugIndexOffset = 12;
/** A roll-set ordinal outside the ladder: none, or the marker for a plug rolled by no set. */
constexpr std::uint16_t kNoRollSet = 0;
/** Embedded reusable-list array descriptor inside one 80-byte ordinary socket entry. */
constexpr std::size_t kEmbeddedPlugListOffset = 64;
/** Reusable and randomized shared plug-set row indices inside one socket entry. */
constexpr std::size_t kReusablePlugSetIndexOffset = 12;
constexpr std::size_t kRandomizedPlugSetIndexOffset = 32;
/** One shared plug-set table row is 24 bytes and carries its member descriptor at byte 8. */
constexpr std::size_t kPlugSetRowStride = 24;
constexpr std::size_t kPlugSetMemberDescriptorOffset = 8;
/** Both embedded and shared plug member rows name an item index first and occupy 32 bytes. */
constexpr std::size_t kPlugMemberStride = 32;
constexpr std::size_t kPlugMemberIndexOffset = 0;
/** Native arrays use 16-bit definition indices even though their serialized field is 32-bit. */
constexpr std::uint64_t kMaximumPlugMemberCount = 65535;
/** Objective block fields after its objective array descriptor. */
constexpr std::size_t kPrimaryOperationOffset = 0x10;
constexpr std::size_t kSecondaryOperationOffset = 0x12;
constexpr std::size_t kCompletionContextValueOffset = 0x14;
constexpr std::size_t kSuppressCompletionOffset = 0x16;
constexpr std::size_t kRequireAllObjectivesOffset = 0x17;
constexpr std::size_t kRequireNonOptionalCompletionOffset = 0x18;
constexpr std::size_t kAutomaticCompletionOffset = 0x19;
constexpr std::size_t kCompletionValueOffset = 0x1A;
/** A bounty declares its lifetime as a one-row expression array at this field. */
constexpr std::size_t kLifetimeExpressionField = 192;
/** Item +0x98 holds the reward-preview block offset relative to that field; zero means absent. */
constexpr std::size_t kRewardPreviewBlockOffset = 0x98;
constexpr std::uint32_t kRewardPreviewBlockClass = 0x80807875U;
/** The block's 32-bit declared count precedes every fixed slot, declared or not. */
constexpr std::size_t kRewardPreviewSlotsOffset = sizeof(std::uint32_t);
/** One slot is a 32-bit item index, a signed quantity, then a 32-bit condition index. */
constexpr std::size_t kRewardPreviewSlotStride = 12;
constexpr std::size_t kRewardPreviewQuantityOffset = 4;
constexpr std::size_t kRewardPreviewConditionOffset = 8;
/** The class ID of a serialized block sits immediately before its payload. */
constexpr std::size_t kBlockClassPrefixSize = sizeof(std::uint32_t);

/**
 * Reads the socket entry list index a definition declares.
 * @param definition Whole item definition bytes.
 * @param row Receives list presence and index.
 */
void read_socket_entry_list(std::span<const std::byte> definition, Row& row) noexcept {
    row.hasSocketEntryList = false;
    row.socketEntryListIndex = 0;
    std::int64_t relative = 0;
    if (!read(definition, kSocketEntryListBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kSocketEntryListBlockOffset) + relative;
    if (block < 0) {
        return;
    }
    const auto blockOffset = static_cast<std::uint64_t>(block);
    if (blockOffset + kSocketEntryListBlockSize > definition.size()) {
        return;
    }
    std::uint16_t index = 0;
    if (read(definition, static_cast<std::size_t>(block), index)) {
        row.hasSocketEntryList = true;
        row.socketEntryListIndex = index;
    }
}

/**
 * Reads the optional equipment slot a definition declares.
 * @param definition Whole item definition bytes.
 * @param slot Receives the slot when the block is present and readable.
 * @param raw Receives the unvalidated declared value, or -1 when the block is absent.
 */
void read_equipment_slot(std::span<const std::byte> definition,
                         std::optional<std::int8_t>& slot,
                         std::int32_t& raw) noexcept {
    slot.reset();
    raw = -1;
    std::int64_t relative = 0;
    if (!read(definition, kEquipmentBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kEquipmentBlockOffset) + relative;
    if (block < 0) {
        return;
    }
    // A value outside the equippable range marks an item that takes no slot.
    std::int32_t value = 0;
    if (read(definition, static_cast<std::size_t>(block) + kEquipmentSlotOffset, value)
        && value >= 0 && value < static_cast<std::int32_t>(kEquipmentSlotCount)) {
        slot = static_cast<std::int8_t>(value);
    }
}

/**
 * Reads the ordinary socket lanes a definition declares.
 * The lane's type index picks the overflow-hash priority the character record applies, so it is
 * read from the same entry as the plug rather than worked out later.
 * @param definition Whole item definition bytes.
 * @param row Receives socket presence, count, types and initial plugs.
 */
void read_sockets(std::span<const std::byte> definition, Row& row) noexcept {
    row.hasSockets = false;
    row.socketCount = 0;
    std::fill(std::begin(row.initialPlugs), std::end(row.initialPlugs), kUnavailablePlug);
    std::fill(std::begin(row.socketTypes), std::end(row.socketTypes), kUnavailableSocketType);
    std::int64_t relative = 0;
    if (!read(definition, kSocketBlockOffset, relative) || relative == 0) {
        return;
    }
    const std::int64_t block = static_cast<std::int64_t>(kSocketBlockOffset) + relative;
    if (block < 0 || static_cast<std::uint64_t>(block) >= definition.size()) {
        return;
    }
    Array array{};
    if (!find_array_at(definition, static_cast<std::size_t>(block), array)
        || array.elementClass != kOrdinarySocketClass) {
        return;
    }
    row.hasSockets = true;
    const auto lanes =
        static_cast<std::size_t>((std::min)(array.count, std::uint64_t{kSocketCapacity}));
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        const std::size_t entry = array.dataOffset + lane * kSocketEntryStride;
        std::uint16_t plug = kUnavailablePlug;
        std::uint16_t type = kUnavailableSocketType;
        if (!read(definition, entry + kSocketPlugOffset, plug)
            || !read(definition, entry + kSocketTypeOffset, type)) {
            return;
        }
        row.initialPlugs[lane] = plug;
        row.socketTypes[lane] = type;
        row.socketCount = static_cast<std::uint8_t>(lane + 1);
    }
}

/** Walks one checked array of 32-byte plug rows. */
[[nodiscard]] bool visit_plug_array(std::span<const std::byte> blob,
                                    const Array& array,
                                    AllowedPlugVisitor visitor,
                                    void* context) noexcept {
    if (array.count > kMaximumPlugMemberCount || array.dataOffset > blob.size()
        || array.count > (blob.size() - array.dataOffset) / kPlugMemberStride) {
        return false;
    }
    for (std::uint64_t index = 0; index < array.count; ++index) {
        std::uint32_t itemDefinitionIndex = 0;
        const std::size_t row =
            array.dataOffset + static_cast<std::size_t>(index) * kPlugMemberStride;
        if (!read(blob, row + kPlugMemberIndexOffset, itemDefinitionIndex)
            || !visitor(context, itemDefinitionIndex)) {
            return false;
        }
    }
    return true;
}

/** Walks one reusable or randomized shared plug-set row, when the socket declares it. */
[[nodiscard]] bool visit_shared_plug_set(std::span<const std::byte> definition,
                                         std::size_t socketEntry,
                                         std::size_t setIndexOffset,
                                         std::span<const std::byte> plugSetTable,
                                         const Array& sets,
                                         AllowedPlugVisitor visitor,
                                         void* context) noexcept {
    std::uint16_t setIndex = kUnavailablePlug;
    if (!read(definition, socketEntry + setIndexOffset, setIndex)) {
        return false;
    }
    if (setIndex == kUnavailablePlug) {
        return true;
    }
    if (setIndex >= sets.count || sets.dataOffset > plugSetTable.size()
        || sets.count > (plugSetTable.size() - sets.dataOffset) / kPlugSetRowStride) {
        return false;
    }
    const std::size_t descriptor = sets.dataOffset
                                   + static_cast<std::size_t>(setIndex) * kPlugSetRowStride
                                   + kPlugSetMemberDescriptorOffset;
    std::uint64_t memberCount = 0;
    if (!read(plugSetTable, descriptor, memberCount)) {
        return false;
    }
    if (memberCount == 0) {
        return true;
    }
    Array members{};
    return find_array_at(plugSetTable, descriptor, members)
           && visit_plug_array(plugSetTable, members, visitor, context);
}

/** The block header carries its own self-relative pointer to the entries at byte 8. */
constexpr std::size_t kStatDataMember = 8;
/** One stat entry is 40 blob bytes. */
constexpr std::size_t kStatEntryStride = 40;
/** A stat entry names its stat table row at byte 16. */
constexpr std::size_t kStatEntryRow = 16;
/** A stat entry carries its signed contribution at byte 20. */
constexpr std::size_t kStatEntryValue = 20;

/**
 * Reads the per-item stat contributions declared inside the definition blob.
 * Two self-relative hops: the definition points at a header, the header points at the entries.
 * Both are blob offsets and are valid only against the serialized definition.
 * @param definition Whole item definition bytes.
 * @param row Receives the declared stat rows and values.
 */
/** @return The offset of the first record of one class inside a window, or the blob size. */
[[nodiscard]] std::size_t find_record(std::span<const std::byte> definition,
                                      std::uint32_t recordClass,
                                      std::size_t start,
                                      std::size_t end) noexcept {
    const std::size_t limit = (std::min)(end, definition.size());
    for (std::size_t offset = start; offset + sizeof(std::uint32_t) <= limit;
         offset += sizeof(std::uint32_t)) {
        std::uint32_t marker = 0;
        if (read(definition, offset, marker) && marker == recordClass) {
            return offset;
        }
    }
    return definition.size();
}

/**
 * Reads the located plug block: its category (which corrects the fixed-offset read for a shifted
 * block), the ordinal of the server roll set that grants the plug, and any linked plug.
 */
void read_plug_block(std::span<const std::byte> definition, Row& row) noexcept {
    row.rollSetIndex = kNoRollSet;
    row.linkedPlugIndex = kUnavailablePlug;
    const std::size_t block =
        find_record(definition, kPlugBlockClass, kPlugBlockSearchStart, kPlugBlockSearchEnd);
    if (block < definition.size()) {
        (void)read(definition, block + kPlugBlockCategoryOffset, row.plugCategoryHash);
        (void)read(definition, block + kPlugBlockRollSetOffset, row.rollSetIndex);
    }
    const std::size_t linked = find_record(definition, kLinkedPlugClass, 0, definition.size());
    if (linked < definition.size()) {
        (void)read(definition, linked + kLinkedPlugIndexOffset, row.linkedPlugIndex);
    }
}

/**
 * Reads the investment stat block into the row.
 * @param definition Whole item definition record.
 * @param row Receives the stats, left empty when the block is absent or malformed.
 */
void read_stats(std::span<const std::byte> definition, Row& row) noexcept {
    row.statCount = 0;
    std::int64_t blockRelative = 0;
    if (!read(definition, kStatBlockOffset, blockRelative)) {
        return;
    }
    const auto header = static_cast<std::int64_t>(kStatBlockOffset) + blockRelative;
    if (header < 0 || static_cast<std::size_t>(header) + kStatDataMember > definition.size()) {
        return;
    }

    std::uint64_t count = 0;
    std::int64_t dataRelative = 0;
    if (!read(definition, static_cast<std::size_t>(header), count) || count == 0
        || count > kStatCapacity
        || !read(definition, static_cast<std::size_t>(header) + kStatDataMember, dataRelative)) {
        return;
    }
    const std::int64_t data = header + static_cast<std::int64_t>(kStatDataMember) + dataRelative;
    if (data < 0 || static_cast<std::size_t>(data) + kStatEntryStride * count > definition.size()) {
        return;
    }

    for (std::uint64_t entry = 0; entry < count; ++entry) {
        const auto base = static_cast<std::size_t>(data) + kStatEntryStride * entry;
        if (!read(definition, base + kStatEntryRow, row.statRows[entry])
            || !read(definition, base + kStatEntryValue, row.statValues[entry])) {
            row.statCount = 0;
            return;
        }
    }
    row.statCount = static_cast<std::uint8_t>(count);
}

/**
 * Locates an optional class-tagged block through its self-relative field.
 * @param present Receives whether the field names a block; false with success means absent.
 * @return False when the field is unreadable or names an out-of-bounds or wrong-class block.
 */
[[nodiscard]] bool optional_block(std::span<const std::byte> definition,
                                  std::size_t field,
                                  std::uint32_t expectedClass,
                                  std::size_t& block,
                                  bool& present) noexcept {
    present = false;
    std::int64_t delta = 0;
    if (!read(definition, field, delta)) {
        return false;
    }
    if (delta == 0) {
        return true;
    }
    std::uint32_t actualClass = 0;
    if (!relative(definition, field, block) || block < kBlockClassPrefixSize
        || !read(definition, block - kBlockClassPrefixSize, actualClass)
        || actualClass != expectedClass) {
        return false;
    }
    present = true;
    return true;
}

/** Reads one serialized boolean, refusing any byte other than zero or one. */
[[nodiscard]] bool
read_boolean(std::span<const std::byte> definition, std::size_t offset, bool& value) noexcept {
    std::uint8_t raw = 0;
    if (!read(definition, offset, raw) || raw > 1) {
        return false;
    }
    value = raw != 0;
    return true;
}

/** A declared slot names an item; every slot past the count carries the empty pattern. */
[[nodiscard]] bool valid_reward_slot(bool declared,
                                     std::uint32_t item,
                                     std::int32_t quantity,
                                     std::uint32_t condition) noexcept {
    if (item > kAbsentReference || condition > kAbsentReference || quantity < 0) {
        return false;
    }
    return declared ? item != kAbsentReference
                    : item == kAbsentReference && condition == kAbsentReference && quantity == 0;
}

/** Reads every reward-preview slot; one malformed slot refuses the whole preview. */
[[nodiscard]] bool read_reward_metadata(std::span<const std::byte> definition, Row& row) noexcept {
    row.rewardCount = 0;
    std::size_t block = 0;
    bool present = false;
    if (!optional_block(
            definition, kRewardPreviewBlockOffset, kRewardPreviewBlockClass, block, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    std::uint32_t count = 0;
    if (!read(definition, block, count) || count > kRewardCapacity) {
        return false;
    }
    for (std::size_t slot = 0; slot < kRewardCapacity; ++slot) {
        const std::size_t at = block + kRewardPreviewSlotsOffset + slot * kRewardPreviewSlotStride;
        std::uint32_t item = 0;
        std::uint32_t condition = 0;
        std::int32_t quantity = 0;
        if (!read(definition, at, item)
            || !read(definition, at + kRewardPreviewQuantityOffset, quantity)
            || !read(definition, at + kRewardPreviewConditionOffset, condition)
            || !valid_reward_slot(slot < count, item, quantity, condition)) {
            return false;
        }
        if (slot < count) {
            row.rewards[slot] = {
                static_cast<std::uint16_t>(item), static_cast<std::uint16_t>(condition), quantity};
        }
    }
    row.rewardCount = static_cast<std::uint8_t>(count);
    return true;
}

/** Reads the completion controls and native value references of one objective block. */
[[nodiscard]] bool read_completion_metadata(std::span<const std::byte> definition,
                                            std::size_t block,
                                            Row& row) noexcept {
    if (!read(definition, block + kCompletionContextValueOffset, row.completionContextValueIndex)
        || !read(definition, block + kCompletionValueOffset, row.completionValueIndex)
        || !read_boolean(definition, block + kSuppressCompletionOffset, row.suppressCompletion)
        || !read_boolean(definition, block + kRequireAllObjectivesOffset, row.requireAllObjectives)
        || !read_boolean(definition,
                         block + kRequireNonOptionalCompletionOffset,
                         row.requireNonOptionalCompletion)
        || !read_boolean(definition, block + kAutomaticCompletionOffset, row.automaticCompletion)) {
        return false;
    }
    row.hasCompletionMetadata = true;
    return true;
}

/** A bounty's lifetime expression holds one row whose operand is the lifetime in seconds. */
[[nodiscard]] bool read_lifetime(std::span<const std::byte> definition, Row& row) noexcept {
    Array lifetime{};
    if (!find_optional_array_at(definition, kLifetimeExpressionField, lifetime)) {
        return false;
    }
    return lifetime.count == 0
           || (lifetime.count == 1 && lifetime.elementClass == kInvestmentExpressionRowClass
               && read(definition,
                       lifetime.dataOffset + kUnlockInstructionOperandOffset,
                       row.lifetimeSeconds)
               && row.lifetimeSeconds >= 0);
}

/** Reads a pursuit's objective block, completion controls and lifetime. */
[[nodiscard]] bool read_pursuit_metadata(std::span<const std::byte> definition, Row& row) noexcept {
    // Other definitions reuse this field for other block types, equipment among them.
    if (row.bucketId != state::build_data::items::kPursuitBucketId || row.equipmentSlot.has_value()
        || row.maxStackSize > 1) {
        return true;
    }
    std::size_t block = 0;
    bool present = false;
    if (!optional_block(
            definition, kItemObjectiveBlockOffset, kItemObjectiveBlockClass, block, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    Array objectives{};
    if (!read(definition, block + kPrimaryOperationOffset, row.primaryOperationIndex)
        || !read(definition, block + kSecondaryOperationOffset, row.secondaryOperationIndex)
        || !read_completion_metadata(definition, block, row)
        || !find_optional_array_at(definition, block, objectives)) {
        return false;
    }
    if (objectives.count == 0) {
        return true;
    }
    if (objectives.elementClass != kObjectiveReferenceArrayClass
        || objectives.count > kObjectiveCapacity) {
        return false;
    }
    for (std::size_t i = 0; i < objectives.count; ++i) {
        if (!read(definition,
                  objectives.dataOffset + i * kObjectiveReferenceStride,
                  row.objectiveIndices[i])) {
            return false;
        }
    }
    row.objectiveCount = static_cast<std::uint8_t>(objectives.count);
    return read_lifetime(definition, row);
}

} // namespace

/** Reads the fixed fields of one item definition blob. */
bool read_definition(std::span<const std::byte> definition, Row& row) noexcept {
    const std::uint32_t hash = row.definitionHash;
    const std::uint16_t index = row.definitionIndex;
    row = {};
    row.definitionHash = hash;
    row.definitionIndex = index;
    row.insertionMaterialRequirementSetIndex = kUnavailableMaterialRequirementSetIndex;
    row.enabledMaterialRequirementSetIndex = kUnavailableMaterialRequirementSetIndex;
    std::fill(std::begin(row.initialPlugs), std::end(row.initialPlugs), kUnavailablePlug);
    std::fill(std::begin(row.socketTypes), std::end(row.socketTypes), kUnavailableSocketType);
    if (definition.size() < kFixedFieldEnd) {
        return false;
    }
    std::uint8_t instanced = 0;
    std::uint8_t quietAcquisition = 0;
    std::uint8_t toastlessAcquisition = 0;
    if (!read(definition, kBucketIdOffset, row.bucketId)
        || !read(definition, kMaxStackSizeOffset, row.maxStackSize)
        || !read(definition, kTierOffset, row.tier)
        || !read(definition, kInstancedOffset, instanced)
        || !read(definition, kRecoveryBucketOffset, row.recoveryBucketId)
        || !read(definition, kStackUniqueLabelOffset, row.stackUniqueLabel)
        || !read(definition, kQuietAcquisitionOffset, quietAcquisition)
        || !read(definition, kToastlessAcquisitionOffset, toastlessAcquisition)
        || quietAcquisition > 1 || toastlessAcquisition > 1) {
        return false;
    }
    row.instanced = instanced != 0;
    row.quietAcquisition = quietAcquisition != 0;
    row.toastlessAcquisition = toastlessAcquisition != 0;
    // Short legacy definitions simply do not declare a plug category.
    (void)read(definition, kPlugCategoryOffset, row.plugCategoryHash);
    read_plug_block(definition, row);
    (void)read(definition,
               kInsertionMaterialRequirementSetIndexOffset,
               row.insertionMaterialRequirementSetIndex);
    (void)read(definition,
               kEnabledMaterialRequirementSetIndexOffset,
               row.enabledMaterialRequirementSetIndex);
    read_stats(definition, row);
    read_appearance(definition, row);
    read_socket_entry_list(definition, row);
    read_equipment_slot(definition, row.equipmentSlot, row.rawEquipmentSlot);
    row.objectiveMetadataValid = read_pursuit_metadata(definition, row);
    row.rewardMetadataValid = row.objectiveMetadataValid && row.objectiveCount != 0
                              && read_reward_metadata(definition, row);
    read_sockets(definition, row);
    return true;
}

/** Visits every list-backed allowed plug for one exact ordinary socket lane. */
bool visit_allowed_plugs(std::span<const std::byte> definition,
                         std::span<const std::byte> plugSetTable,
                         std::uint8_t lane,
                         AllowedPlugVisitor visitor,
                         void* context) noexcept {
    if (visitor == nullptr || lane >= kSocketCapacity) {
        return false;
    }
    std::int64_t socketBlockRelative = 0;
    if (!read(definition, kSocketBlockOffset, socketBlockRelative) || socketBlockRelative == 0) {
        return false;
    }
    const std::int64_t socketBlock =
        static_cast<std::int64_t>(kSocketBlockOffset) + socketBlockRelative;
    if (socketBlock < 0 || static_cast<std::uint64_t>(socketBlock) >= definition.size()) {
        return false;
    }
    Array sockets{};
    if (!find_array_at(definition, static_cast<std::size_t>(socketBlock), sockets)
        || sockets.elementClass != kOrdinarySocketClass || lane >= sockets.count
        || sockets.dataOffset > definition.size()
        || sockets.count > (definition.size() - sockets.dataOffset) / kSocketEntryStride) {
        return false;
    }
    Array sets{};
    if (!find_array_at(plugSetTable, kTableArrayDescriptor, sets)) {
        return false;
    }
    const std::size_t socketEntry =
        sockets.dataOffset + static_cast<std::size_t>(lane) * kSocketEntryStride;
    Array embedded{};
    std::uint64_t embeddedCount = 0;
    if (!read(definition, socketEntry + kEmbeddedPlugListOffset, embeddedCount)
        || (embeddedCount != 0
            && (!find_array_at(definition, socketEntry + kEmbeddedPlugListOffset, embedded)
                || !visit_plug_array(definition, embedded, visitor, context)))) {
        return false;
    }
    return visit_shared_plug_set(definition,
                                 socketEntry,
                                 kReusablePlugSetIndexOffset,
                                 plugSetTable,
                                 sets,
                                 visitor,
                                 context)
           && visit_shared_plug_set(definition,
                                    socketEntry,
                                    kRandomizedPlugSetIndexOffset,
                                    plugSetTable,
                                    sets,
                                    visitor,
                                    context);
}

} // namespace sunrise::middleware::content::packages::tables::items
