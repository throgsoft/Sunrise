#include "reward_catalog.h"

#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "../../unlocks/definition.h"
#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::rewards {
namespace {

core::threading::SrwLock g_lock;
Table<Pool, kPoolCapacity> g_pools;
Table<Entry, kEntryCapacity> g_entries;
Table<Item, kItemCapacity> g_items;
Table<Instruction, kInstructionCapacity> g_instructions;
Table<Modifier, kModifierCapacity> g_modifiers;
Table<SocketOverride, kSocketOverrideCapacity> g_sockets;

View view() noexcept {
    return {g_pools.rows(),
            g_entries.rows(),
            g_items.rows(),
            g_instructions.rows(),
            g_modifiers.rows(),
            g_sockets.rows()};
}

bool acyclic(View data,
             std::size_t index,
             std::array<std::uint8_t, kPoolCapacity>& visited,
             std::size_t depth = 0) noexcept {
    if (depth >= kTraversalDepth) return false;
    if (visited[index] != 0) {
        return visited[index] == 2;
    }
    visited[index] = 1;
    const Range range = data.pools[index].entries;
    for (const Entry& entry : data.entries.subspan(range.first, range.count)) {
        if (entry.poolIndex != kAbsent && !acyclic(data, entry.poolIndex, visited, depth + 1)) {
            return false;
        }
    }
    visited[index] = 2;
    return true;
}

} // namespace

void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_pools.clear();
    g_entries.clear();
    g_items.clear();
    g_instructions.clear();
    g_modifiers.clear();
    g_sockets.clear();
}

bool ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_pools.count() != 0 && g_items.count() != 0;
}

bool valid(View data) noexcept {
    if (data.pools.empty() || data.pools.size() > kPoolCapacity || data.entries.empty()
        || data.entries.size() > kEntryCapacity || data.items.empty()
        || data.items.size() > kItemCapacity || data.instructions.size() > kInstructionCapacity
        || data.modifiers.size() > kModifierCapacity
        || data.sockets.size() > kSocketOverrideCapacity) {
        return false;
    }
    for (const Pool& pool : data.pools) {
        if (pool.definitionHash == 0 || !fits(pool.entries, data.entries)) {
            return false;
        }
    }
    for (const Entry& entry : data.entries) {
        if ((entry.itemIndex != kAbsent && entry.itemIndex >= data.items.size())
            || (entry.poolIndex != kAbsent && entry.poolIndex >= data.pools.size())
            || !fits(entry.condition, data.instructions) || !fits(entry.modifiers, data.modifiers)
            || !fits(entry.sockets, data.sockets) || !std::isfinite(entry.weight)
            || !std::isfinite(entry.scale) || entry.weight < 0 || entry.scale < 0) {
            return false;
        }
    }
    for (const Item& item : data.items) {
        if (item.definitionHash == 0 || item.selectionCount > item.selections.size()
            || (item.acquiredFlag != kAbsent && item.acquiredFlag >= unlocks::kAccountFlagCapacity)
            || (item.poolIndex != kAbsent && item.poolIndex >= data.pools.size())) {
            return false;
        }
    }
    for (const Instruction& instruction : data.instructions) {
        const auto kind = static_cast<BankRead>(instruction.opcode);
        const auto slot = instruction.operand;
        if ((kind == BankRead::accountFlag && slot >= unlocks::kAccountFlagCapacity)
            || (kind == BankRead::characterFlag && slot >= unlocks::kCharacterObjectFlagCapacity)
            || (kind == BankRead::accountValue && slot >= unlocks::kObjectiveValueCapacity)
            || (kind == BankRead::characterValue && slot >= unlocks::kCharacterObjectValueCapacity))
            return false;
    }
    for (const Modifier& modifier : data.modifiers) {
        if (!fits(modifier.condition, data.instructions) || !std::isfinite(modifier.value)) {
            return false;
        }
    }
    for (const SocketOverride& socket : data.sockets) {
        if (socket.socketType == kAbsent
            || (socket.plugItem != kAbsent && socket.plugItem >= data.items.size())) {
            return false;
        }
    }
    std::array<std::uint8_t, kPoolCapacity> visited{};
    for (std::size_t index = 0; index < data.pools.size(); ++index) {
        if (!acyclic(data, index, visited)) {
            return false;
        }
    }
    return true;
}

bool replace(View data) noexcept {
    if (!valid(data)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_pools.replace(data.pools) && g_entries.replace(data.entries)
           && g_items.replace(data.items) && g_instructions.replace(data.instructions)
           && g_modifiers.replace(data.modifiers) && g_sockets.replace(data.sockets);
}

bool read(void* context, bool (*consume)(void*, View) noexcept) noexcept {
    const std::shared_lock guard(g_lock);
    return consume != nullptr && g_pools.count() != 0 && consume(context, view());
}

bool find_item(std::uint16_t itemIndex, Item& item) noexcept {
    item = {};
    const std::shared_lock guard(g_lock);
    if (itemIndex >= g_items.count()) {
        return false;
    }
    item = g_items.rows()[itemIndex];
    return true;
}

bool snapshot(std::span<Pool> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_pools.snapshot(output, count);
}

bool snapshot(std::span<Entry> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_entries.snapshot(output, count);
}

bool snapshot(std::span<Item> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_items.snapshot(output, count);
}

bool snapshot(std::span<Instruction> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_instructions.snapshot(output, count);
}

bool snapshot(std::span<Modifier> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_modifiers.snapshot(output, count);
}

bool snapshot(std::span<SocketOverride> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_sockets.snapshot(output, count);
}

} // namespace sunrise::state::build_data::rewards
