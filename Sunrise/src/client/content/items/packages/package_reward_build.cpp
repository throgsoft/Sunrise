#include "package_reward_build.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/rewards/reward_catalog.h"
#include "../../../../state/build_data/runtime.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::rewards;

/** Investment-root slots identify reward pools and unlock-slot bindings. */
constexpr std::size_t kPoolSlot = 88;
constexpr std::size_t kFlagSlot = 112;
constexpr std::size_t kValueSlot = 114;
/** Reward tables store 24-byte pool rows and 80-byte entries. */
constexpr std::uint32_t kPoolClass = 0x80807553U;
constexpr std::uint32_t kPoolRowClass = 0x8080748CU;
constexpr std::uint32_t kEntryClass = 0x8080748EU;
constexpr std::uint32_t kExpressionClass = 0x80807D31U;
constexpr std::uint32_t kModifierClass = 0x80807490U;
constexpr std::uint32_t kSocketClass = 0x80803062U;
constexpr std::uint32_t kWrapperClass = 0x808077CCU;
constexpr std::uint32_t kSelectionClass = 0x808077CFU;
/** Item headers hold a relative wrapper pointer and an acquired-unlock slot. */
constexpr std::size_t kWrapperField = 0x58;
constexpr std::size_t kAcquiredFlagField = 0xDA;

template <typename T>
bool field(std::span<const std::byte> blob, std::size_t at, T& output) noexcept {
    if (at > blob.size() || sizeof output > blob.size() - at) {
        return false;
    }
    std::memcpy(&output, blob.data() + at, sizeof output);
    return true;
}

bool array(std::span<const std::byte> blob,
           std::size_t at,
           std::uint32_t cls,
           std::size_t stride,
           tables::Array& rows) noexcept {
    return tables::find_optional_array_at(blob, at, rows)
           && (rows.count == 0
               || (rows.elementClass == cls && rows.dataOffset <= blob.size()
                   && rows.count <= (blob.size() - rows.dataOffset) / stride));
}

template <typename T> bool append(std::vector<T>& bank, T value, std::size_t capacity) noexcept {
    if (bank.size() == capacity) {
        return false;
    }
    try {
        bank.push_back(value);
        return true;
    } catch (...) {
        return false;
    }
}

struct Builder {
    std::vector<domain::Pool> pools;
    std::vector<domain::Entry> entries;
    std::vector<domain::Item> items;
    std::vector<domain::Instruction> instructions;
    std::vector<domain::Modifier> modifiers;
    std::vector<domain::SocketOverride> sockets;
    RewardConditions conditions;

    bool expression(std::span<const std::byte> blob, std::size_t at, domain::Range& out) noexcept {
        return conditions.read(blob, at, instructions, out);
    }

    bool entry(std::span<const std::byte> blob, std::size_t at) noexcept {
        domain::Entry out{};
        // Fixed fields precede condition, weight-modifier, and socket arrays.
        if (!field(blob, at, out.itemIndex) || !field(blob, at + 2, out.itemType)
            || !field(blob, at + 4, out.quantity) || !field(blob, at + 8, out.poolIndex)
            || !field(blob, at + 10, out.mappingIndex) || !field(blob, at + 12, out.scale)
            || !field(blob, at + 16, out.adjusterIndex) || !field(blob, at + 20, out.categoryHash)
            || !field(blob, at + 24, out.weight) || !field(blob, at + 28, out.bucketHash)
            || !expression(blob, at + 32, out.condition)) {
            return false;
        }
        tables::Array rows{};
        if (!array(blob, at + 48, kModifierClass, 24, rows)) {
            return false;
        }
        out.modifiers = {static_cast<std::uint32_t>(modifiers.size()),
                         static_cast<std::uint32_t>(rows.count)};
        for (std::size_t i = 0; i < rows.count; ++i) {
            const auto offset = rows.dataOffset + i * 24;
            domain::Modifier modifier{};
            if (!expression(blob, offset, modifier.condition)
                || !field(blob, offset + 16, modifier.valueIndex)
                || !field(blob, offset + 20, modifier.value)
                || !append(modifiers, modifier, domain::kModifierCapacity)) {
                return false;
            }
        }
        std::array<domain::SocketOverride, 12> overrides{};
        std::size_t count = 0;
        if (!read_reward_sockets(blob, at + 64, overrides, count)
            || count > domain::kSocketOverrideCapacity - sockets.size()) {
            return false;
        }
        out.sockets = {static_cast<std::uint32_t>(sockets.size()),
                       static_cast<std::uint32_t>(count)};
        for (std::size_t i = 0; i < count; ++i) {
            if (!append(sockets, overrides[i], domain::kSocketOverrideCapacity)) return false;
        }
        return append(entries, out, domain::kEntryCapacity);
    }
};

bool root_table(const reader::Source& source,
                reader::Scratch& scratch,
                std::span<const std::byte> root,
                std::size_t slot,
                std::vector<std::byte>& blob,
                std::uint32_t expectedClass = 0) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t cls = 0;
    return tables::slot_tag(root, slot, tag) && tables::package_of(tag) != tables::kAbsentPackageId
           && reader::read_tag(source, scratch, tag, blob, cls)
           && (expectedClass == 0 || cls == expectedClass);
}

} // namespace

bool RewardConditions::load(const reader::Source& source,
                            reader::Scratch& scratch,
                            std::span<const std::byte> root) noexcept {
    // Root slot 109 holds expressions referenced by native operator 12.
    return root_table(source, scratch, root, kFlagSlot, flags_, 0x80807D49U)
           && array(flags_, tables::kTableArrayDescriptor, 0x80807D4FU, 8, flagRows_)
           && root_table(source, scratch, root, kValueSlot, values_, 0x80807C92U)
           && array(values_, tables::kTableArrayDescriptor, 0x80807C96U, 8, valueRows_)
           && root_table(source, scratch, root, 109, expressions_, 0x80807C49U)
           && array(expressions_, tables::kTableArrayDescriptor, 0x80807C4FU, 24, expressionRows_);
}

bool RewardConditions::bind(domain::Instruction& instruction) const noexcept {
    const bool flag = instruction.opcode == 1;
    if (!flag && instruction.opcode != 10) {
        return true;
    }
    const auto& rows = flag ? flagRows_ : valueRows_;
    const std::span<const std::byte> blob = flag ? flags_ : values_;
    if (instruction.operand >= rows.count) {
        return false;
    }
    const std::size_t at = rows.dataOffset + instruction.operand * 8;
    std::uint32_t hash = 0;
    std::uint16_t scope = 0;
    std::uint16_t index = domain::kAbsent;
    if (!field(blob, at, hash) || !field(blob, at + 4, scope) || !field(blob, at + 6, index)) {
        return false;
    }
    using B = domain::BankRead;
    const B kind = scope == 1   ? (flag ? B::accountFlag : B::accountValue)
                   : scope == 2 ? (flag ? B::characterFlag : B::characterValue)
                                : (flag ? B::externalFlag : B::externalValue);
    instruction.opcode = static_cast<std::uint32_t>(kind);
    instruction.operand = scope == 1 || scope == 2 ? index : hash;
    return true;
}

bool RewardConditions::append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<domain::Instruction>& bank,
                                         std::size_t depth) const noexcept {
    tables::Array rows{};
    if (depth >= domain::kTraversalDepth || !array(blob, at, kExpressionClass, 8, rows))
        return false;
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Instruction instruction{};
        if (!field(blob, rows.dataOffset + i * 8, instruction.opcode)
            || !field(blob, rows.dataOffset + i * 8 + 4, instruction.operand))
            return false;
        if (instruction.opcode == 12) {
            const auto before = bank.size();
            if (instruction.operand >= expressionRows_.count
                || !append_expression(expressions_,
                                      expressionRows_.dataOffset + instruction.operand * 24 + 8,
                                      bank,
                                      depth + 1)
                || bank.size() == before)
                return false;
        } else if (!bind(instruction) || !append(bank, instruction, domain::kInstructionCapacity)) {
            return false;
        }
    }
    return true;
}

bool RewardConditions::read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<domain::Instruction>& bank,
                            domain::Range& range) const noexcept {
    const auto first = bank.size();
    if (!append_expression(blob, at, bank, 0)) {
        bank.resize(first);
        return false;
    }
    range = {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bank.size() - first)};
    return true;
}

bool RewardConditions::read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<domain::Instruction> output,
                                 std::size_t& count) const noexcept {
    count = 0;
    tables::Array rows{};
    std::vector<domain::Instruction> instructions;
    if (!array(blob, at, 0x80807D2FU, 16, rows)) return false;
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Range expression{};
        if (!read(blob, rows.dataOffset + i * 16, instructions, expression)
            || expression.count == 0) {
            return false;
        }
        // Every expression attached to a progression reward must hold.
        if (i != 0) instructions.push_back({4, 0});
        if (instructions.size() > output.size()) return false;
    }
    std::copy(instructions.begin(), instructions.end(), output.begin());
    count = instructions.size();
    return true;
}

bool read_reward_sockets(std::span<const std::byte> blob,
                         std::size_t at,
                         std::span<domain::SocketOverride> output,
                         std::size_t& count) noexcept {
    count = 0;
    tables::Array rows{};
    if (!array(blob, at, kSocketClass, 12, rows) || rows.count > output.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t p = rows.dataOffset + i * 12;
        auto& socket = output[i];
        if (!field(blob, p, socket.socketType) || !field(blob, p + 2, socket.plugItem)
            || !field(blob, p + 4, socket.plugSet) || !field(blob, p + 6, socket.rollSet)
            || !field(blob, p + 8, socket.selection)) {
            return false;
        }
    }
    count = static_cast<std::size_t>(rows.count);
    return true;
}

bool build_rewards(const reader::Source& source,
                   reader::Scratch& scratch,
                   std::span<const std::byte> root) noexcept {
    if (domain::ready()) {
        return true;
    }
    Builder build{};
    std::vector<std::byte> blob;
    std::vector<std::byte> index;
    tables::Array pools{};
    tables::Array items{};
    if (!build.conditions.load(source, scratch, root)
        || !root_table(source, scratch, root, kPoolSlot, blob, kPoolClass)
        || !array(blob, tables::kTableArrayDescriptor, kPoolRowClass, 24, pools) || pools.count == 0
        || pools.count > domain::kPoolCapacity
        || !root_table(source, scratch, root, tables::kItemTableSlot, index)
        || !array(index, tables::kTableArrayDescriptor, tables::kItemIndexTableClass, 24, items)
        || items.count == 0 || items.count > domain::kItemCapacity) {
        return false;
    }
    for (std::size_t i = 0; i < pools.count; ++i) {
        const std::size_t at = pools.dataOffset + i * 24;
        domain::Pool pool{};
        tables::Array entries{};
        if (!field(blob, at, pool.definitionHash)
            || !array(blob, at + 8, kEntryClass, 80, entries)) {
            return false;
        }
        pool.entries = {static_cast<std::uint32_t>(build.entries.size()),
                        static_cast<std::uint32_t>(entries.count)};
        for (std::size_t j = 0; j < entries.count; ++j) {
            if (!build.entry(blob, entries.dataOffset + j * 80)) {
                return false;
            }
        }
        if (!append(build.pools, pool, domain::kPoolCapacity)) return false;
    }
    for (std::size_t i = 0; i < items.count; ++i) {
        tables::IndexRow row{};
        domain::Item item{};
        std::uint32_t cls = 0;
        std::uint16_t acquired = domain::kAbsent;
        std::int64_t relative = 0;
        if (!tables::index_row(index, items, i, row)
            || !reader::read_tag(source, scratch, row.targetTag, blob, cls)
            || cls != tables::kItemDefinitionClass || !field(blob, kWrapperField, relative)
            || !field(blob, kAcquiredFlagField, acquired)) {
            return false;
        }
        item.definitionHash = row.definitionHash;
        if (acquired != domain::kAbsent) {
            domain::Instruction flag{1, acquired};
            if (!build.conditions.bind(flag)) {
                return false;
            }
            if (flag.opcode == static_cast<std::uint32_t>(domain::BankRead::accountFlag)) {
                item.acquiredFlag = static_cast<std::uint16_t>(flag.operand);
            }
        }
        if (relative != 0) {
            if (relative < 0 || static_cast<std::uint64_t>(relative) > blob.size()
                || kWrapperField > blob.size() - static_cast<std::size_t>(relative)) {
                return false;
            }
            const auto at = kWrapperField + static_cast<std::size_t>(relative);
            tables::Array selections{};
            if (!field(blob, at - 4, cls) || cls != kWrapperClass
                || !field(blob, at, item.poolIndex) || !field(blob, at + 4, item.categoryHash)
                || !field(blob, at + 24, item.flags)
                || !array(blob, at + 8, kSelectionClass, 12, selections)
                || selections.count > item.selections.size()) {
                return false;
            }
            item.selectionCount = static_cast<std::uint8_t>(selections.count);
            for (std::size_t j = 0; j < selections.count; ++j) {
                auto& selection = item.selections[j];
                const auto p = selections.dataOffset + j * 12;
                if (!field(blob, p, selection.categoryHash) || !field(blob, p + 4, selection.count)
                    || !field(blob, p + 8, selection.policy)) {
                    return false;
                }
            }
        }
        if (!append(build.items, item, domain::kItemCapacity)) return false;
    }
    return state::build_data::publish_reward_definitions({build.pools,
                                                          build.entries,
                                                          build.items,
                                                          build.instructions,
                                                          build.modifiers,
                                                          build.sockets});
}

} // namespace sunrise::client::content::items::packages
