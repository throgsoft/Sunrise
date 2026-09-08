#include "dawning_oven_runtime.h"

#include <algorithm>
#include <limits>

#include "../../core/runtime/wall_clock.h"
#include "dawning_bounty_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::dawning {
namespace identity = account::inventory::dawning;
namespace items = build_data::items;
namespace materials = build_data::material_requirements;
namespace buckets = build_data::inventory::buckets;
namespace {

struct Recipe {
    items::Definition plug{};
    materials::Definition costs{};
    std::size_t first{identity::kIngredientCount}, second{identity::kIngredientCount};
    std::size_t firstCost{identity::kIngredientCount}, secondCost{identity::kIngredientCount};
    materials::Requirement essence{};
};

bool resolve_recipe(std::size_t index, bool discounted, Recipe& result) noexcept {
    if (index >= identity::kRecipes.size()) return false;
    Recipe recipe{};
    const auto& mapping = identity::kRecipes[index];
    const auto hash = discounted ? mapping.masterworkedPlugHash : mapping.plugHash;
    items::Definition oven{};
    if (!build_data::find_item_definition_hash(identity::kOvenHash, oven)
        || !build_data::find_item_definition_hash(hash, recipe.plug)
        || !build_data::is_socket_plug_allowed(oven.definitionIndex, 3, recipe.plug.definitionIndex)
        || !build_data::find_material_requirement_set(
            recipe.plug.insertionMaterialRequirementSetIndex, recipe.costs)
        || recipe.costs.requirementSetIndex != recipe.plug.insertionMaterialRequirementSetIndex
        || recipe.costs.requirementCount != 3)
        return false;
    for (std::size_t i = 0; i < recipe.costs.requirementCount; ++i) {
        const auto& cost = recipe.costs.requirements[i];
        items::Definition material{};
        if (!cost.deleteOnAction || cost.omitFromRequirements || cost.quantity == 0
            || !build_data::find_item_definition_index(cost.itemDefinitionIndex, material))
            return false;
        if (material.definitionHash == identity::kEssenceHash) {
            if (recipe.essence.quantity != 0
                || cost.condition != materials::kUnconditionalRequirement
                || cost.quantity
                       > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
                return false;
            recipe.essence = cost;
            continue;
        }
        const auto ingredient = identity::ingredient(material.definitionHash);
        if (!ingredient_installed(ingredient) || cost.quantity != 1
            || material.definitionHash != identity::kIngredients[ingredient].plugHash)
            return false;
        // The native selector is the actual debit, even when the displayed material differs.
        // These twenty retained kind-1 value definitions map to account rows 204..223.
        if (cost.condition < 512 || cost.condition >= 512 + identity::kIngredientCount)
            return false;
        const auto balance = static_cast<std::size_t>(cost.condition - 512);
        if ((ingredient < 6) != (balance < 6) || !ingredient_installed(balance)) return false;
        auto& destination = ingredient < 6 ? recipe.first : recipe.second;
        auto& debit = ingredient < 6 ? recipe.firstCost : recipe.secondCost;
        if (destination != identity::kIngredientCount) return false;
        destination = ingredient;
        debit = balance;
    }
    if (recipe.first >= 6 || recipe.second < 6 || recipe.second >= identity::kIngredientCount
        || recipe.essence.quantity == 0)
        return false;
    result = recipe;
    return true;
}

bool profile_stack(std::uint32_t hash,
                   items::Definition& item,
                   items::details::Definition& detail) noexcept {
    buckets::Descriptor bucket{};
    return build_data::find_item_definition_hash(hash, item) && item.definitionHash == hash
           && build_data::find_configured_item_detail(item.definitionIndex, detail)
           && detail.definitionIndex == item.definitionIndex && detail.definitionHash == hash
           && detail.bucketId == item.bucketId && detail.maxStackSize > 0
           && detail.instancedDefinitionState == items::details::InstancedDefinitionState::stackable
           && !detail.equipmentSlot.has_value()
           && build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
           && bucket.arraySelector == buckets::ArraySelector::profile
           && !build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
}
} // namespace

bool stage(const AccountState& snapshot,
           std::size_t characterIndex,
           std::uint64_t targetInstanceSoid,
           std::uint8_t socketLane,
           std::uint16_t plugDefinitionIndex,
           PendingSocketPlug& mutation) noexcept {
    mutation = {};
    if (!account::valid(snapshot) || !valid_profile_inventory(snapshot)
        || characterIndex >= snapshot.characterCount || socketLane < 2 || socketLane > 4
        || !snapshot.characters[characterIndex].selected)
        return false;
    const auto& character = snapshot.characters[characterIndex];
    CharacterItemLocation location{};
    if (!find_character_item_location(character, targetInstanceSoid, location)) return false;
    const auto* target = character_item_at(character, location);
    items::Definition oven{}, requested{};
    items::details::Definition detail{};
    if (!target || target->definitionHash != identity::kOvenHash || target->quantity != 1
        || !build_data::find_item_definition_hash(target->definitionHash, oven)
        || !build_data::find_configured_item_detail(oven.definitionIndex, detail)
        || detail.definitionHash != oven.definitionHash
        || detail.definitionIndex != oven.definitionIndex || detail.bucketId != oven.bucketId
        || detail.ordinarySocketCount != 5
        || detail.ordinarySocketState != items::details::OrdinarySocketState::present
        || !build_data::find_item_definition_index(plugDefinitionIndex, requested)
        || !build_data::is_socket_plug_allowed(
            oven.definitionIndex, socketLane, plugDefinitionIndex))
        return false;
    auto sockets = target->sockets;
    account::inventory::Sockets defaults{};
    defaults.policy = account::inventory::SocketPolicy::authored;
    defaults.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < defaults.plugCount; ++lane) {
        items::Definition plug{};
        if (!build_data::find_item_definition_index(detail.initialPlugIndices[lane], plug))
            return false;
        defaults.plugs[lane] = plug.definitionHash;
    }
    if (sockets.policy == account::inventory::SocketPolicy::nativeDefaults) sockets = defaults;
    if (sockets.policy != account::inventory::SocketPolicy::authored || sockets.plugCount != 5
        || !account::inventory::valid(sockets))
        return false;
    State before{};
    if (!read(before)) return false;
    auto after = before;
    after.initialized = true;
    AccountState candidate = snapshot;
    materials::Definition costs{};
    const bool discounted = sockets.plugs[4] == identity::kMasterworkHash;
    if (!discounted && sockets.plugs[4] != defaults.plugs[4]) return false;

    if (socketLane == 4) {
        if (requested.definitionHash != identity::kMasterworkHash || discounted
            || !std::all_of(before.recipes.begin(), before.recipes.end(), [](auto flag) {
                   return flag == unlocks::kFlagSet;
               }))
            return false;
        if (requested.insertionMaterialRequirementSetIndex != materials::kUnavailableSetIndex
            && (!build_data::find_material_requirement_set(
                    requested.insertionMaterialRequirementSetIndex, costs)
                || costs.requirementSetIndex != requested.insertionMaterialRequirementSetIndex
                || costs.requirementCount != 0))
            return false;
        sockets.plugs[4] = identity::kMasterworkHash;
    } else {
        std::size_t chosen = identity::kRecipes.size();
        Recipe recipe{};
        if (socketLane == 3) {
            for (std::size_t i = 0; i < identity::kRecipes.size(); ++i) {
                const auto& row = identity::kRecipes[i];
                if (requested.definitionHash
                    == (discounted ? row.masterworkedPlugHash : row.plugHash))
                    chosen = i;
            }
            if (chosen == identity::kRecipes.size() || before.recipes[chosen] != unlocks::kFlagSet
                || !resolve_recipe(chosen, discounted, recipe))
                return false;
            if (discounted) {
                Recipe ordinary{};
                if (!resolve_recipe(chosen, false, ordinary) || ordinary.first != recipe.first
                    || ordinary.second != recipe.second || ordinary.firstCost != recipe.firstCost
                    || ordinary.secondCost != recipe.secondCost
                    || ordinary.essence.quantity <= recipe.essence.quantity)
                    return false;
            }
        } else {
            if (requested.definitionHash != identity::kCombineHash || discounted) return false;
            const auto first = identity::ingredient(sockets.plugs[0].value_or(0));
            const auto second = identity::ingredient(sockets.plugs[1].value_or(0));
            if (first >= 6 || second < 6 || second >= identity::kIngredientCount
                || sockets.plugs[0] != identity::kIngredients[first].plugHash
                || sockets.plugs[1] != identity::kIngredients[second].plugHash
                || !ingredient_installed(first) || !ingredient_installed(second))
                return false;
            // Resolve the complete installed menu before declaring a pair unmatched (burnt).
            for (std::size_t i = 0; i < identity::kRecipes.size(); ++i) {
                Recipe option{};
                if (!resolve_recipe(i, false, option)) return false;
                if (option.first == first && option.second == second) {
                    if (chosen != identity::kRecipes.size()) return false;
                    chosen = i;
                    recipe = option;
                }
            }
            if (chosen == identity::kRecipes.size()) {
                recipe.first = first;
                recipe.second = second;
                recipe.firstCost = first;
                recipe.secondCost = second;
                if (!build_data::find_material_requirement_set(
                        requested.insertionMaterialRequirementSetIndex, recipe.costs)
                    || recipe.costs.requirementSetIndex
                           != requested.insertionMaterialRequirementSetIndex
                    || recipe.costs.requirementCount != 1)
                    return false;
                recipe.essence = recipe.costs.requirements[0];
                items::Definition essence{};
                if (!build_data::find_item_definition_index(recipe.essence.itemDefinitionIndex,
                                                            essence)
                    || essence.definitionHash != identity::kEssenceHash
                    || recipe.essence.quantity == 0 || !recipe.essence.deleteOnAction
                    || recipe.essence.omitFromRequirements
                    || recipe.essence.condition != materials::kUnconditionalRequirement)
                    return false;
            }
        }
        if (after.ingredients[recipe.firstCost] < 1 || after.ingredients[recipe.secondCost] < 1)
            return false;
        costs = recipe.costs;
        materials::Definition charge = costs;
        charge.requirementCount = 1;
        charge.requirements = {};
        charge.requirements[0] = recipe.essence;
        items::Definition essence{}, cookie{};
        items::details::Definition essenceDetail{}, cookieDetail{};
        const auto cookieHash = chosen == identity::kRecipes.size()
                                    ? identity::kBurntCookieHash
                                    : identity::kRecipes[chosen].cookieHash;
        bool charged = false;
        if (!profile_stack(identity::kEssenceHash, essence, essenceDetail)
            || !profile_stack(cookieHash, cookie, cookieDetail)
            || !apply_action_materials(snapshot, charge, candidate, charged) || !charged)
            return false;
        PendingProfileItemAcquisition grant{};
        if (!finalize_profile_item_acquisition(
                candidate, candidate, cookieHash, cookieDetail, false, 1, {.direct = true}, grant))
            return false;
        // The Essence debit may compact a pre-existing cookie row, or remove the greatest
        // serial. Grant against that compacted view but keep acquisition ordering above
        // every row the client saw before payment.
        std::int32_t greatest = 0;
        for (std::size_t i = 0; i < snapshot.profileItemCount; ++i)
            greatest = (std::max)(greatest, snapshot.profileItems[i].mutationSerial);
        auto& baked = grant.afterItems[grant.profileIndex];
        if (baked.mutationSerial <= greatest) {
            if (greatest == (std::numeric_limits<std::int32_t>::max)()) return false;
            baked.mutationSerial = greatest + 1;
        }
        candidate.profileItems = grant.afterItems;
        candidate.profileItemCount = grant.afterItemCount;
        --after.ingredients[recipe.firstCost];
        --after.ingredients[recipe.secondCost];
        if (chosen != identity::kRecipes.size()) after.recipes[chosen] = unlocks::kFlagSet;
        if (chosen != identity::kRecipes.size()
            && !credit_bounties(candidate.characters[characterIndex],
                                false,
                                cookieHash,
                                core::runtime::investment_clock_seconds()))
            return false;
        for (std::size_t lane = 0; lane < 4; ++lane)
            sockets.plugs[lane] = defaults.plugs[lane];
    }
    auto* changed = character_item_at(candidate.characters[characterIndex], location);
    if (!changed) return false;
    changed->sockets = sockets;
    middleware::datagen::family4::loadout::ResolvedLoadout beforeLoadout{}, afterLoadout{};
    ResolvedPosition beforePosition{}, afterPosition{};
    if (!account::valid(candidate) || !valid_profile_inventory(candidate)
        || !middleware::datagen::family4::loadout::resolve(snapshot, characterIndex, beforeLoadout)
        || !middleware::datagen::family4::loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition))
        return false;
    items::Definition result{};
    if (!build_data::find_item_definition_hash(sockets.plugs[socketLane].value_or(0), result))
        return false;
    mutation.beforeCharacter = character;
    mutation.afterCharacter = candidate.characters[characterIndex];
    mutation.beforeProfileItems = snapshot.profileItems;
    mutation.afterProfileItems = candidate.profileItems;
    mutation.beforeDawning = before;
    mutation.afterDawning = after;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = character.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.targetDefinitionHash = oven.definitionHash;
    mutation.plugDefinitionHash = result.definitionHash;
    mutation.materialRequirementSetHash = costs.requirementSetHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedProfileItemCount = snapshot.profileItemCount;
    mutation.afterProfileItemCount = candidate.profileItemCount;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = oven.definitionIndex;
    mutation.plugDefinitionIndex = result.definitionIndex;
    mutation.requestedPlugDefinitionIndex = plugDefinitionIndex;
    mutation.materialRequirementSetIndex = costs.requirementSetIndex;
    mutation.socketLane = socketLane;
    mutation.targetBucketId = oven.bucketId;
    mutation.plugBucketId = result.bucketId;
    mutation.materialRequirementCount = costs.requirementCount;
    mutation.profileChanged = !same_profile_views(snapshot.profileItems,
                                                  snapshot.profileItemCount,
                                                  candidate.profileItems,
                                                  candidate.profileItemCount);
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return true;
}
} // namespace sunrise::state::runtime::detail::dawning
