#include "crafting_metadata_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "../../../../state/build_data/items/details/definition.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables::crafting {
namespace data = sunrise::state::build_data::crafting;
using data::ChalicePlug;
using data::Expression;
using data::kChaliceFlagSlots;
using data::kChaliceHash;
using data::kChaliceIndex;
using data::kChaliceSocketTypes;
using data::kQuestValue;
using data::kRuneValueBase;
using data::kUpgradeFlagBase;
using data::SynthesizerCosts;
namespace {
namespace tables = sunrise::middleware::content::packages::tables;
namespace items = sunrise::state::build_data::items;

// Package record classes and byte offsets; these are file layouts, not executable addresses.
constexpr std::uint32_t kFlagSlotClass = 0x80807D4FU;
constexpr std::uint32_t kValueSlotClass = 0x80807C96U;
constexpr std::uint32_t kFlagMapClass = 0x80807D48U;
constexpr std::uint32_t kValueMapClass = 0x80807C8DU;
constexpr std::uint32_t kPlugBlockClass = 0x808077E3U;
constexpr std::uint32_t kPlugRuleClass = 0x808077E6U;
constexpr std::uint32_t kCostScalarClass = 0x80805D86U;
constexpr std::uint32_t kItemUnlockClass = 0x808077ABU;
constexpr std::uint32_t kEmittedFlagClass = 0x80807D4BU;
constexpr std::size_t kPlugBlockPointer = 64;
constexpr std::size_t kPlugRulesDescriptor = 64;
constexpr std::size_t kPlugQuantityExpression = 224;
constexpr std::size_t kSocketScalarsDescriptor = 56;
constexpr std::size_t kCostScalarStride = 8;
constexpr std::size_t kItemUnlockPointer = 144;
// The two subsequent unlock lists must be empty: only emitted flags are handled here.
constexpr std::size_t kItemUnlockSecondList = 16;
constexpr std::size_t kItemUnlockThirdList = 32;
// Chalice source slots: twelve rune quantities, two selected-rune values, and quest progress.
constexpr std::uint16_t kFirstRuneValueSlot = 5512;
constexpr std::uint16_t kFirstSelectedRuneSlot = 5503;
constexpr std::uint16_t kQuestValueSlot = 12823;
constexpr std::size_t kRuneCount = 12;
// This build's supported emitted-flag slot range is checked before Family-5 projection.
constexpr std::uint16_t kEmittedFlagSlotLimit = 23500;

/** Definition hashes paired with the Chalice socket-type indices in lane order. */
constexpr std::array<std::uint32_t, 8> kSocketHashes{2609924932U,
                                                     1250700346U,
                                                     3195245497U,
                                                     3276661397U,
                                                     3422549977U,
                                                     3422549976U,
                                                     3422549979U,
                                                     1639095227U};
/** Weak, middling, powerful, and recycling socket types, identified by installed hash. */
constexpr std::array<std::uint32_t, 4> kSynthSocketTypes{
    3533862596U, 3533862597U, 3533862598U, 3396161649U};

template <typename T>
bool read(std::span<const std::byte> bytes, std::size_t at, T& value) noexcept {
    if (at > bytes.size() || sizeof(T) > bytes.size() - at) {
        return false;
    }
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return true;
}

bool array(std::span<const std::byte> bytes,
           std::size_t at,
           std::uint32_t cls,
           std::size_t stride,
           tables::Array& result) noexcept {
    return tables::find_array_at(bytes, at, result) && result.elementClass == cls
           && result.dataOffset <= bytes.size()
           && result.count <= (bytes.size() - result.dataOffset) / stride;
}

bool expression(std::span<const std::byte> bytes, std::size_t at, Expression& result) noexcept {
    tables::Array code{};
    if (!array(bytes,
               at,
               tables::kInvestmentExpressionRowClass,
               tables::kUnlockInstructionStride,
               code)
        || code.count == 0 || code.count > result.code.size()) {
        return false;
    }
    result.count = static_cast<std::size_t>(code.count);
    for (std::size_t i = 0; i < result.count; ++i) {
        if (!read(bytes, code.dataOffset + i * tables::kUnlockInstructionStride, result.code[i])) {
            return false;
        }
    }
    return true;
}
} // namespace

bool read_chalice_banks(std::span<const std::byte> flagSlots,
                        std::span<const std::byte> valueSlots,
                        std::span<const std::byte> flagMaps,
                        std::span<const std::byte> valueMaps) noexcept {
    tables::Array flags{}, values{}, flagMap{}, valueMap{};
    if (!array(flagSlots,
               tables::kTableArrayDescriptor,
               kFlagSlotClass,
               tables::kUnlockSlotRowStride,
               flags)
        || !array(valueSlots,
                  tables::kTableArrayDescriptor,
                  kValueSlotClass,
                  tables::kUnlockSlotRowStride,
                  values)
        || !array(flagMaps,
                  tables::kTableArrayDescriptor,
                  kFlagMapClass,
                  tables::kUnlockMapRowStride,
                  flagMap)
        || !array(valueMaps,
                  tables::kTableArrayDescriptor,
                  kValueMapClass,
                  tables::kUnlockMapRowStride,
                  valueMap)) {
        return false;
    }
    const auto mapping = [](auto sourceBytes,
                            const auto& sources,
                            auto mapBytes,
                            const auto& maps,
                            std::uint16_t slot,
                            std::uint16_t bank,
                            std::uint8_t aggregation) noexcept {
        std::uint8_t kind{}, flags{};
        std::uint16_t mapped{}, reverse{};
        std::uint32_t sourceHash{}, mapHash{};
        const auto at = sources.dataOffset + static_cast<std::size_t>(slot) * 8;
        const auto dest = maps.dataOffset + static_cast<std::size_t>(bank) * 8;
        return slot < sources.count && bank < maps.count && read(sourceBytes, at, sourceHash)
               && read(mapBytes, dest, mapHash) && sourceHash == mapHash
               && read(sourceBytes, at + 4, kind) && kind == 1 && read(sourceBytes, at + 5, flags)
               && flags == aggregation && read(sourceBytes, at + 6, mapped) && mapped == bank
               && read(mapBytes, dest + 4, reverse) && reverse == slot;
    };
    for (std::size_t i = 0; i < kChaliceFlagSlots.size(); ++i) {
        if (!mapping(flagSlots,
                     flags,
                     flagMaps,
                     flagMap,
                     kChaliceFlagSlots[i],
                     static_cast<std::uint16_t>(kUpgradeFlagBase + i),
                     0)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kRuneCount; ++i) {
        if (!mapping(valueSlots,
                     values,
                     valueMaps,
                     valueMap,
                     static_cast<std::uint16_t>(kFirstRuneValueSlot + i),
                     static_cast<std::uint16_t>(kRuneValueBase + i),
                     0)) {
            return false;
        }
    }
    for (std::uint16_t slot = kFirstSelectedRuneSlot; slot < kFirstSelectedRuneSlot + 2; ++slot) {
        std::uint8_t kind{}, aggregation{};
        std::uint16_t bank{};
        const auto at = values.dataOffset + static_cast<std::size_t>(slot) * 8;
        if (slot >= values.count || !read(valueSlots, at + 4, kind) || kind != 4
            || !read(valueSlots, at + 5, aggregation) || aggregation != 0
            || !read(valueSlots, at + 6, bank) || bank != 0xFFFFU) {
            return false;
        }
    }
    if (!mapping(valueSlots, values, valueMaps, valueMap, kQuestValueSlot, kQuestValue, 2)) {
        return false;
    }
    return true;
}

bool read_chalice_sockets(std::span<const std::byte> bytes) noexcept {
    tables::Array types{};
    if (!array(bytes,
               tables::kTableArrayDescriptor,
               tables::kSocketTypeTableClass,
               tables::kSocketTypeRowStride,
               types)) {
        return false;
    }
    for (std::size_t lane = 0; lane < kChaliceSocketTypes.size(); ++lane) {
        const auto at = types.dataOffset + kChaliceSocketTypes[lane] * tables::kSocketTypeRowStride;
        std::uint32_t hash{};
        tables::Array scalars{};
        if (kChaliceSocketTypes[lane] >= types.count || !read(bytes, at, hash)
            || hash != kSocketHashes[lane]
            || !tables::find_optional_array_at(bytes, at + kSocketScalarsDescriptor, scalars)
            || scalars.count != 0) {
            return false;
        }
    }
    return true;
}

bool read_chalice_item(std::uint16_t index,
                       std::uint32_t hash,
                       std::span<const std::byte> bytes,
                       ChalicePlug& output) noexcept {
    if (!data::needs_chalice_item(index) || hash == 0) {
        return false;
    }
    output = {};
    if (index == kChaliceIndex) {
        if (hash != kChaliceHash) {
            return false;
        }
    } else {
        std::int64_t relative{};
        std::uint32_t cls{};
        if (!read(bytes, kPlugBlockPointer, relative) || relative <= 0
            || bytes.size() < kPlugBlockPointer
            || static_cast<std::uint64_t>(relative) > bytes.size() - kPlugBlockPointer) {
            return false;
        }
        const auto at = kPlugBlockPointer + static_cast<std::size_t>(relative);
        if (!read(bytes, at - 4, cls) || cls != kPlugBlockClass) {
            return false;
        }
        tables::Array rules{};
        if (!tables::find_optional_array_at(bytes, at + kPlugRulesDescriptor, rules)
            || rules.count > output.rules.size()
            || (rules.count != 0
                && (rules.elementClass != kPlugRuleClass || rules.dataOffset > bytes.size()
                    || rules.count > (bytes.size() - rules.dataOffset)
                                         / tables::kUnlockExpressionFieldSize))) {
            return false;
        }
        output.ruleCount = static_cast<std::size_t>(rules.count);
        for (std::size_t i = 0; i < output.ruleCount; ++i) {
            if (!expression(bytes,
                            rules.dataOffset + i * tables::kUnlockExpressionFieldSize,
                            output.rules[i])) {
                return false;
            }
        }
        if (index >= 7949 && index <= 7984) {
            Expression quantity{};
            if (!expression(bytes, at + kPlugQuantityExpression, quantity) || quantity.count != 1
                || quantity.code[0].op != tables::kUnlockReadValueOpcode
                || quantity.code[0].operand != kFirstRuneValueSlot + (index - 7949U) % kRuneCount) {
                return false;
            }
        }
    }
    output.hash = hash;
    output.ready = true;
    return true;
}

bool read_synthesizer_costs(std::span<const std::byte> bytes, SynthesizerCosts& output) noexcept {
    tables::Array table{};
    if (!tables::find_array_at(bytes, tables::kTableArrayDescriptor, table)
        || table.elementClass != tables::kSocketTypeTableClass || table.count == 0
        || table.count > (std::numeric_limits<std::uint16_t>::max)()
        || table.dataOffset > bytes.size()
        || table.count > (bytes.size() - table.dataOffset) / tables::kSocketTypeRowStride) {
        return false;
    }
    SynthesizerCosts costs{};
    std::array<bool, kSynthSocketTypes.size()> found{};
    for (std::size_t i = 0; i < table.count; ++i) {
        const auto at = table.dataOffset + i * tables::kSocketTypeRowStride;
        std::uint32_t hash{};
        if (!read(bytes, at, hash)) {
            return false;
        }
        const auto match = std::find(kSynthSocketTypes.begin(), kSynthSocketTypes.end(), hash);
        if (match == kSynthSocketTypes.end()) {
            continue;
        }
        const auto tier = static_cast<std::size_t>(match - kSynthSocketTypes.begin());
        if (found[tier]) {
            return false;
        }
        found[tier] = true;
        auto& cost = costs[tier];
        cost.socketType = static_cast<std::uint16_t>(i);
        tables::Array scalars{};
        if (!tables::find_optional_array_at(bytes, at + kSocketScalarsDescriptor, scalars)
            || scalars.count > cost.scalars.size()
            || (scalars.count != 0
                && (scalars.elementClass != kCostScalarClass || scalars.dataOffset > bytes.size()
                    || scalars.count > (bytes.size() - scalars.dataOffset) / kCostScalarStride))) {
            return false;
        }
        cost.count = static_cast<std::size_t>(scalars.count);
        for (std::size_t j = 0; j < cost.count; ++j) {
            auto& scalar = cost.scalars[j];
            float value{};
            std::uint16_t reserved{};
            if (!read(bytes, scalars.dataOffset + j * kCostScalarStride, scalar.itemIndex)
                || !read(bytes, scalars.dataOffset + j * kCostScalarStride + 2, reserved)
                || reserved != 0
                || !read(bytes, scalars.dataOffset + j * kCostScalarStride + 4, value)
                || scalar.itemIndex == items::details::kUnavailableItemIndex
                || !std::isfinite(value) || value < 1 || std::floor(value) != value
                || static_cast<double>(value) > (std::numeric_limits<std::int32_t>::max)()) {
                return false;
            }
            scalar.multiplier = static_cast<std::uint32_t>(value);
            for (std::size_t prior = 0; prior < j; ++prior) {
                if (cost.scalars[prior].itemIndex == scalar.itemIndex) {
                    return false;
                }
            }
        }
    }
    if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) {
        return false;
    }
    output = costs;
    return true;
}

bool read_mote_output_flags(std::span<const std::byte> bytes,
                            std::span<const std::byte> flagSlots,
                            std::array<std::uint16_t, 2>& output) noexcept {
    // Item +144 names unlock_item_block, class 808077AB. Its first list contains
    // emitted flag slots (class 80807D4B, u16 entries), not per-instance item flags.
    std::int64_t relative{};
    if (!read(bytes, kItemUnlockPointer, relative) || relative <= 0
        || static_cast<std::uint64_t>(relative) > bytes.size()
        || bytes.size() - static_cast<std::size_t>(relative) < kItemUnlockPointer) {
        return false;
    }
    const auto block = kItemUnlockPointer + static_cast<std::size_t>(relative);
    std::uint32_t cls{};
    tables::Array flags{}, slots{}, other{};
    if (!read(bytes, block - 4, cls) || cls != kItemUnlockClass
        || !tables::find_array_at(bytes, block, flags) || flags.elementClass != kEmittedFlagClass
        || flags.count != 2 || flags.dataOffset > bytes.size()
        || bytes.size() - flags.dataOffset < 4
        || !tables::find_optional_array_at(bytes, block + kItemUnlockSecondList, other)
        || other.count != 0
        || !tables::find_optional_array_at(bytes, block + kItemUnlockThirdList, other)
        || other.count != 0
        || !tables::find_array_at(flagSlots, tables::kTableArrayDescriptor, slots)
        || slots.elementClass != kFlagSlotClass || slots.dataOffset > flagSlots.size()
        || slots.count > (flagSlots.size() - slots.dataOffset) / 8) {
        return false;
    }
    std::array<std::uint16_t, 2> slotsOutput{};
    for (std::size_t i = 0; i < slotsOutput.size(); ++i) {
        std::uint8_t kind{}, scope{};
        std::uint16_t bank{};
        if (!read(bytes, flags.dataOffset + i * 2, slotsOutput[i]) || slotsOutput[i] >= slots.count
            || slotsOutput[i] >= kEmittedFlagSlotLimit) {
            return false;
        }
        const auto row = slots.dataOffset + static_cast<std::size_t>(slotsOutput[i]) * 8;
        if (!read(flagSlots, row + 4, kind) || kind != 0 || !read(flagSlots, row + 5, scope)
            || scope != 0 || !read(flagSlots, row + 6, bank) || bank != 0xFFFFU) {
            return false;
        }
    }
    if (slotsOutput[0] == slotsOutput[1]) {
        return false;
    }
    output = slotsOutput;
    return true;
}

} // namespace sunrise::middleware::content::packages::tables::crafting
