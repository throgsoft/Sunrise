#include "reward_resolver.h"

#include <algorithm>
#include <cmath>

#include "../build_data/rewards/reward_catalog.h"

namespace sunrise::state::rewards {
namespace {

namespace definitions = build_data::rewards;
/** The empty bucket tag inherits the enclosing pool's bucket constraint. */
constexpr std::uint32_t kEmptyTag = 0x811C9DC5U;
/** Bounded postfix stack for native reward conditions. */
constexpr std::size_t kExpressionCapacity = 64;

bool condition(definitions::View data,
               definitions::Range expression,
               const Context& context,
               bool& result) noexcept {
    result = expression.count == 0;
    if (!definitions::fits(expression, data.instructions)) {
        return false;
    }
    std::array<std::int64_t, kExpressionCapacity> stack{};
    std::size_t size = 0;
    for (const auto& instruction : data.instructions.subspan(expression.first, expression.count)) {
        const auto operand = instruction.operand;
        std::int64_t value = 0;
        using Read = definitions::BankRead;
        using Op = definitions::Opcode;
        switch (instruction.opcode) {
        case static_cast<std::uint32_t>(Read::accountFlag):
            if (operand >= context.unlocks.accountFlags.size()) return false;
            value = context.unlocks.accountFlags[operand] == unlocks::kFlagSet;
            break;
        case static_cast<std::uint32_t>(Read::characterFlag):
            if (operand >= context.unlocks.characterObjectFlags.size()) return false;
            value = context.unlocks.characterObjectFlags[operand] == unlocks::kFlagSet;
            break;
        case static_cast<std::uint32_t>(Read::accountValue):
            if (operand >= context.unlocks.objectiveValues.size()) return false;
            value = context.unlocks.objectiveValues[operand];
            break;
        case static_cast<std::uint32_t>(Read::characterValue):
            if (operand >= context.unlocks.characterObjectValues.size()) return false;
            value = context.unlocks.characterObjectValues[operand];
            break;
        case static_cast<std::uint32_t>(Read::externalFlag):
            // Computed class unlocks read the selected server character.
            switch (operand) {
            case 3121290652U:
                value = context.characterClass == CharacterClass::hunter;
                break;
            case 1238696360U:
                value = context.characterClass == CharacterClass::titan;
                break;
            case 2678892537U:
                value = context.characterClass == CharacterClass::warlock;
                break;
            default:
                return false;
            }
            break;
        case static_cast<std::uint32_t>(Op::constant):
            value = static_cast<std::int32_t>(operand);
            break;
        case static_cast<std::uint32_t>(Op::logicalNot):
            if (size == 0) return false;
            stack[size - 1] = stack[size - 1] == 0;
            continue;
        case static_cast<std::uint32_t>(Op::negate):
            if (size == 0) return false;
            stack[size - 1] = -stack[size - 1];
            continue;
        case static_cast<std::uint32_t>(Op::logicalOr):
        case static_cast<std::uint32_t>(Op::logicalAnd):
        case static_cast<std::uint32_t>(Op::equal):
        case static_cast<std::uint32_t>(Op::greaterThan):
        case static_cast<std::uint32_t>(Op::greaterOrEqual):
        case static_cast<std::uint32_t>(Op::lessThan): {
            if (size < 2) return false;
            const auto right = stack[--size];
            auto& left = stack[size - 1];
            switch (instruction.opcode) {
            case static_cast<std::uint32_t>(Op::logicalOr):
                left = left != 0 || right != 0;
                break;
            case static_cast<std::uint32_t>(Op::logicalAnd):
                left = left != 0 && right != 0;
                break;
            case static_cast<std::uint32_t>(Op::equal):
                left = left == right;
                break;
            case static_cast<std::uint32_t>(Op::greaterThan):
                left = left > right;
                break;
            case static_cast<std::uint32_t>(Op::greaterOrEqual):
                left = left >= right;
                break;
            case static_cast<std::uint32_t>(Op::lessThan):
                left = left < right;
                break;
            }
            continue;
        }
        default:
            return false;
        }
        if (size == stack.size()) return false;
        stack[size++] = value;
    }
    if (expression.count != 0) {
        if (size != 1) return false;
        result = stack[0] != 0;
    }
    return true;
}

struct Resolver {
    definitions::View data;
    const Context& context;
    Result& result;
    std::uint64_t random;

    double fraction() noexcept {
        // SplitMix64 makes a prepared seed replayable without shared random state.
        random += 0x9E3779B97F4A7C15ULL;
        auto bits = random;
        bits = (bits ^ (bits >> 30)) * 0xBF58476D1CE4E5B9ULL;
        bits = (bits ^ (bits >> 27)) * 0x94D049BB133111EBULL;
        bits ^= bits >> 31;
        return static_cast<double>(bits >> 11) * 0x1.0p-53;
    }

    bool weight(const definitions::Entry& entry,
                std::uint32_t category,
                std::uint32_t bucket,
                std::size_t depth,
                double& output) noexcept {
        output = 0;
        if (entry.categoryHash != category
            || (bucket != kEmptyTag && entry.bucketHash != kEmptyTag
                && entry.bucketHash != bucket)) {
            return true;
        }
        bool enabled = false;
        if (!condition(data, entry.condition, context, enabled)) return false;
        if (!enabled) return true;
        double value = entry.weight;
        if (!definitions::fits(entry.modifiers, data.modifiers)) return false;
        for (const auto& modifier :
             data.modifiers.subspan(entry.modifiers.first, entry.modifiers.count)) {
            if (!condition(data, modifier.condition, context, enabled)) return false;
            if (enabled) {
                if (modifier.valueIndex != definitions::kAbsent) return false;
                value = modifier.value;
            }
        }
        if (!std::isfinite(value) || value < 0) return false;
        if (value == 0) return true;
        if (entry.poolIndex != definitions::kAbsent) {
            double total = 0;
            if (!pool_weight(entry.poolIndex,
                             category,
                             entry.bucketHash == kEmptyTag ? bucket : entry.bucketHash,
                             depth + 1,
                             total))
                return false;
            if (total == 0) return true;
        } else if (entry.itemIndex != definitions::kAbsent) {
            if (entry.itemIndex >= data.items.size()) return false;
            for (std::size_t i = 0; i < result.count; ++i) {
                if (result.grants[i].itemIndex == entry.itemIndex) return true;
            }
        } else {
            return false;
        }
        output = value;
        return true;
    }

    bool pool_weight(std::uint16_t index,
                     std::uint32_t category,
                     std::uint32_t bucket,
                     std::size_t depth,
                     double& total) noexcept {
        total = 0;
        if (depth >= definitions::kTraversalDepth || index >= data.pools.size()) return false;
        const auto range = data.pools[index].entries;
        if (!definitions::fits(range, data.entries)) return false;
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, bucket, depth, value)) return false;
            total += value;
        }
        return std::isfinite(total);
    }

    bool draw(std::uint16_t index,
              std::uint32_t category,
              std::uint32_t bucket,
              std::size_t depth,
              double total) noexcept {
        const auto range = data.pools[index].entries;
        double remaining = fraction() * total;
        const definitions::Entry* chosen = nullptr;
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, bucket, depth, value)) return false;
            if (value == 0) continue;
            chosen = &entry;
            remaining -= value;
            if (remaining < 0) break;
        }
        if (chosen == nullptr
            || (chosen->quantity != 1 && chosen->poolIndex != definitions::kAbsent)) {
            return false;
        }
        if (chosen->poolIndex != definitions::kAbsent) {
            const auto childBucket = chosen->bucketHash == kEmptyTag ? bucket : chosen->bucketHash;
            double childTotal = 0;
            return pool_weight(chosen->poolIndex, category, childBucket, depth + 1, childTotal)
                   && childTotal > 0
                   && draw(chosen->poolIndex, category, childBucket, depth + 1, childTotal);
        }
        if (chosen->quantity == 0 || chosen->quantity > INT32_MAX
            || result.count == result.grants.size()
            || !definitions::fits(chosen->sockets, data.sockets))
            return false;
        auto& grant = result.grants[result.count++];
        grant.itemIndex = chosen->itemIndex;
        grant.quantity = static_cast<std::int32_t>(chosen->quantity);
        if (chosen->sockets.count > grant.sockets.size()) return false;
        grant.socketCount = chosen->sockets.count;
        std::copy_n(
            data.sockets.begin() + chosen->sockets.first, grant.socketCount, grant.sockets.begin());
        return true;
    }
};

} // namespace

bool eligible(std::span<const definitions::Instruction> instructions,
              const Context& context,
              bool& result) noexcept {
    definitions::View view{};
    view.instructions = instructions;
    if (instructions.size() > UINT32_MAX) return false;
    return condition(view, {0, static_cast<std::uint32_t>(instructions.size())}, context, result);
}

bool resolve(definitions::View data,
             const Context& context,
             std::uint16_t itemIndex,
             std::uint32_t quantity,
             std::uint32_t category,
             Result& result) noexcept {
    result = {};
    if (itemIndex >= data.items.size() || quantity == 0 || quantity > INT32_MAX) return false;
    const auto& item = data.items[itemIndex];
    Result staged{};
    if (item.poolIndex == definitions::kAbsent) {
        staged.grants[0].itemIndex = itemIndex;
        staged.grants[0].quantity = static_cast<std::int32_t>(quantity);
        staged.count = 1;
    } else {
        if (quantity != 1 || item.selectionCount == 0
            || item.selectionCount > item.selections.size()) {
            return false;
        }
        Resolver resolver{data, context, staged, context.seed};
        for (std::size_t i = 0; i < item.selectionCount; ++i) {
            const auto& selection = item.selections[i];
            if (category != 0 && category != selection.categoryHash) continue;
            if (selection.count > staged.grants.size()) return false;
            for (std::size_t j = 0; j < selection.count; ++j) {
                double total = 0;
                if (!resolver.pool_weight(
                        item.poolIndex, selection.categoryHash, kEmptyTag, 0, total)) {
                    return false;
                }
                // A fixed bundle can have fewer eligible members after an acquisition unlock.
                if (total == 0) break;
                if (!resolver.draw(item.poolIndex, selection.categoryHash, kEmptyTag, 0, total)) {
                    return false;
                }
            }
        }
        if (staged.count == 0) return false;
    }
    result = staged;
    return true;
}

bool resolve(const Context& context,
             std::uint16_t itemIndex,
             std::uint32_t quantity,
             std::uint32_t category,
             Result& result) noexcept {
    struct Request {
        const Context& context;
        std::uint16_t item;
        std::uint32_t quantity;
        std::uint32_t category;
        Result& result;
    } request{context, itemIndex, quantity, category, result};
    result = {};
    return definitions::read(&request, [](void* raw, definitions::View data) noexcept {
        auto& value = *static_cast<Request*>(raw);
        return resolve(
            data, value.context, value.item, value.quantity, value.category, value.result);
    });
}

} // namespace sunrise::state::rewards
