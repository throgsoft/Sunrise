#include "dawning_bounty_runtime.h"

#include "../account/inventory/dawning_oven_state.h"
#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail::dawning {
namespace {
struct GiftRule {
    std::uint32_t bountyHash, objectiveHash;
};
// Semantic event joins in recipe order; there are no cached definition indices or target counts.
constexpr std::array<GiftRule, 22> gifts{
    {{3089242929U, 2548740524U}, {2121605528U, 773356093U},  {711780003U, 3624753994U},
     {1387738947U, 596105834U},  {3609320918U, 2454287063U}, {782388144U, 1605382853U},
     {1486111804U, 926975233U},  {3529619172U, 874584025U},  {3292943447U, 2595545358U},
     {3512551717U, 2708783760U}, {1932203931U, 876421010U},  {3112855567U, 2037977526U},
     {1494697569U, 1080833820U}, {789196172U, 355117905U},   {729780848U, 2874539525U},
     {90105468U, 2319574209U},   {1491123344U, 3363642277U}, {3503381224U, 2462970189U},
     {2212074115U, 2889593642U}, {833201720U, 4279320669U},  {2514375981U, 1082188232U},
     {2420899653U, 3129351088U}}};
} // namespace

bool credit_bounties(CharacterState& character,
                     bool delivery,
                     std::uint32_t cookieHash,
                     std::int64_t now) noexcept {
    namespace inventory = account::inventory;
    if (now <= 0 || character.inventory.count > character.inventory.values.size()) return false;
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        auto& held = character.inventory.values[i];
        std::uint32_t objectiveHash{};
        if (!delivery && held.definitionHash == 2568759381U) objectiveHash = 2757754782U;
        if (delivery && held.definitionHash == 2568759380U) objectiveHash = 22889993U;
        if (delivery)
            for (std::size_t recipe = 0; recipe < gifts.size(); ++recipe)
                if (inventory::dawning::kRecipes[recipe].cookieHash == cookieHash
                    && gifts[recipe].bountyHash == held.definitionHash)
                    objectiveHash = gifts[recipe].objectiveHash;
        if (objectiveHash == 0) continue;
        build_data::items::Definition item{};
        build_data::items::details::Definition detail{};
        if (held.instanceSoid == 0 || held.quantity != 1
            || !build_data::find_item_definition_hash(held.definitionHash, item)
            || !build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionHash != held.definitionHash
            || detail.definitionIndex != item.definitionIndex || detail.bucketId != item.bucketId
            || held.objectiveDefinitionIndex != item.definitionIndex || detail.objectiveCount == 0
            || detail.objectiveCount > detail.objectiveIndices.size() || detail.lifetimeSeconds < 0)
            return false;
        if (detail.lifetimeSeconds > 0 && held.objectiveValues[inventory::kItemExpiryLane] <= now)
            continue;
        bool matched = false;
        for (std::size_t entry = 0; entry < detail.objectiveCount; ++entry) {
            build_data::objectives::Definition objective{};
            if (!build_data::find_objective_definition(detail.objectiveIndices[entry], objective))
                return false;
            if (objective.definitionHash != objectiveHash) continue;
            if (matched || objective.completionValue <= 0) return false;
            matched = true;
            auto& progress = held.objectiveValues[inventory::kItemObjectiveLaneBase + entry];
            if (progress < 0) return false;
            if (progress < objective.completionValue) ++progress;
        }
        if (!matched) return false;
    }
    return true;
}
} // namespace sunrise::state::runtime::detail::dawning
