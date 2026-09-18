#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::runtime::detail::bounty {
struct StackRow {
    std::size_t index{};
    std::int32_t quantity{};
};
struct StackCredit {
    std::size_t index{};
    std::int32_t before{}, after{}, credited{};
    bool appended{};
};

/** Plans actual credits, saturating at inventory capacity. Output capacity is not a gameplay cap.
 */
[[nodiscard]] inline bool plan_stack_reward(std::span<const StackRow> matching,
                                            std::int32_t requested,
                                            std::int32_t maximum,
                                            bool allowAdditionalStacks,
                                            std::size_t nextIndex,
                                            std::size_t freeRows,
                                            std::span<StackCredit> output,
                                            std::size_t& count,
                                            std::int32_t& credited) noexcept {
    count = 0;
    credited = 0;
    if (requested <= 0 || maximum <= 0) {
        return false;
    }
    for (std::size_t i = 0; i < matching.size(); ++i) {
        if (matching[i].quantity <= 0 || matching[i].quantity > maximum
            || matching[i].index >= nextIndex) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (matching[j].index == matching[i].index) {
                return false;
            }
        }
    }
    const auto add = [&](std::size_t index, std::int32_t before, bool appended) {
        const auto amount = (std::min)(requested - credited, maximum - before);
        if (amount == 0) {
            return true;
        }
        if (count == output.size()) {
            return false;
        }
        output[count++] = {index, before, before + amount, amount, appended};
        credited += amount;
        return true;
    };
    for (const auto& row : matching) {
        if (!add(row.index, row.quantity, false)) {
            return false;
        }
        if (credited == requested) {
            return true;
        }
    }
    if (!allowAdditionalStacks && !matching.empty()) {
        return true;
    }
    if (!allowAdditionalStacks) {
        freeRows = (std::min)(freeRows, std::size_t{1});
    }
    while (credited < requested && freeRows != 0) {
        if (!add(nextIndex++, 0, true)) {
            return false;
        }
        --freeRows;
    }
    return true;
}
} // namespace sunrise::state::runtime::detail::bounty
