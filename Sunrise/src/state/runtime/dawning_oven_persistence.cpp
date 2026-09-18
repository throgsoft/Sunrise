#include <algorithm>
#include <limits>
#include <mutex>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "dawning_oven_runtime.h"

namespace sunrise::state::runtime::detail::dawning {
namespace identity = account::inventory::dawning;
namespace store = investment::store;
namespace {
constexpr auto kBootstrap = "dawning-earned-recipes-v1";

bool valid(const State& state) noexcept {
    return std::all_of(state.ingredients.begin(),
                       state.ingredients.end(),
                       [](auto value) { return value >= 0; })
           && std::all_of(state.recipes.begin(), state.recipes.end(), [](auto flag) {
                  return flag == 0 || flag == unlocks::kFlagSet;
              });
}
} // namespace

bool read(State& output) noexcept {
    const std::lock_guard lock(store::g_mutex);
    State result{};
    result.initialized = store::bootstrap_completed(kBootstrap);
    for (std::size_t i = 0; i < result.ingredients.size(); ++i) {
        if (!store::read_unlock(store::Bank::objectiveValues,
                                static_cast<std::uint16_t>(identity::kFirstIngredientValue + i),
                                result.ingredients[i]))
            return false;
    }
    // Existing SQLite flags may represent earned progress. Preserve them on adoption;
    // the bootstrap marker cannot establish whether an older all-set default was earned.
    {
        for (std::size_t i = 0; i < result.recipes.size(); ++i) {
            std::int32_t value{};
            if (!store::read_unlock(store::Bank::accountFlags,
                                    static_cast<std::uint16_t>(identity::kFirstRecipeFlag + i),
                                    value)
                || (value != 0 && value != unlocks::kFlagSet))
                return false;
            result.recipes[i] = static_cast<std::uint8_t>(value);
        }
    }
    if (!valid(result)) return false;
    output = result;
    return true;
}

bool write(const State& before, const State& after) noexcept {
    State current{};
    if (!after.initialized || !valid(after) || !read(current) || current != before) return false;
    for (std::size_t i = 0; i < after.ingredients.size(); ++i) {
        if (before.ingredients[i] != after.ingredients[i]
            && !store::write_unlock(store::Bank::objectiveValues,
                                    static_cast<std::uint16_t>(identity::kFirstIngredientValue + i),
                                    after.ingredients[i]))
            return false;
    }
    for (std::size_t i = 0; i < after.recipes.size(); ++i) {
        if (before.recipes[i] != after.recipes[i]
            && !store::write_unlock(store::Bank::accountFlags,
                                    static_cast<std::uint16_t>(identity::kFirstRecipeFlag + i),
                                    after.recipes[i]))
            return false;
    }
    return before.initialized || store::complete_bootstrap(kBootstrap);
}

bool ingredient_installed(std::size_t index) noexcept {
    if (index >= identity::kIngredients.size()) return false;
    const auto& entry = identity::kIngredients[index];
    build_data::items::Definition oven{}, plug{}, pickup{};
    return build_data::find_item_definition_hash(identity::kOvenHash, oven)
           && build_data::find_item_definition_hash(entry.plugHash, plug)
           && build_data::find_item_definition_hash(entry.pickupHash, pickup)
           && oven.definitionHash == identity::kOvenHash && plug.definitionHash == entry.plugHash
           && pickup.definitionHash == entry.pickupHash
           && build_data::is_socket_plug_allowed(
               oven.definitionIndex, index < 6 ? 0 : 1, plug.definitionIndex);
}

bool credit(State& state,
            std::uint32_t definitionHash,
            std::int32_t quantity,
            std::int32_t& credited) noexcept {
    credited = 0;
    const auto index = identity::ingredient(definitionHash);
    if (quantity <= 0 || !valid(state) || !ingredient_installed(index)) return false;
    auto& balance = state.ingredients[index];
    credited = (std::min)(quantity, (std::numeric_limits<std::int32_t>::max)() - balance);
    balance += credited;
    state.initialized = true;
    return true;
}
} // namespace sunrise::state::runtime::detail::dawning
