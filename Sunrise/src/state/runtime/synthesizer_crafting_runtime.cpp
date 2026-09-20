#include "synthesizer_crafting_runtime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

#include "../../core/logging/log.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::synthesizer {
namespace {
namespace items = build_data::items;
namespace materials = build_data::material_requirements;
namespace buckets = build_data::inventory::buckets;
namespace inventory = account::inventory;
namespace crafting = build_data::crafting;

constexpr std::array<std::uint32_t, 3> kContainers{1160544509U, 1160544508U, 1160544511U};
struct Role {
    std::uint32_t synth;
    std::array<std::uint32_t, 3> plugs;
    std::array<std::uint32_t, 3> motes;
    std::array<std::uint32_t, 3> recyclePlugs;
};
// Bounded exchange identities: the socket recipe and the real profile item are distinct.
constexpr std::array<Role, 4> kRoles{{
    {3948022968U,
     {456408693U, 456408692U, 456408695U},
     {3611551832U, 3611551833U, 3611551834U},
     {1431423016U, 1431423017U, 1431423018U}},
    {3552598030U,
     {3728290549U, 3728290548U, 3728290551U},
     {2657400314U, 2657400315U, 2657400312U},
     {1401078730U, 1401078731U, 1401078728U}},
    {1691570586U,
     {3602650887U, 3602650886U, 3602650885U},
     {3162804766U, 3162804767U, 3162804764U},
     {882903534U, 882903535U, 882903532U}},
    {889896758U,
     {1769115309U, 1769115308U, 1769115311U},
     {753747954U, 753747955U, 753747952U},
     {248465794U, 248465795U, 248465792U}},
}};

std::mutex g_costMutex;
crafting::SynthesizerCosts g_costs{};
bool g_costsReady{};

struct MoteOutputFlags {
    std::array<std::uint16_t, 2> slots{};
    bool configured{};
};
std::array<MoteOutputFlags, 12> g_moteOutputs{};

bool output_flags_ready(const std::array<MoteOutputFlags, 12>& outputs) noexcept {
    // Every role emits the shared tier flag plus one distinct role/tier ownership flag.
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (!outputs[i].configured || outputs[i].slots[0] == outputs[i].slots[1]
            || outputs[i].slots[0] != outputs[i % 3].slots[0]) {
            return false;
        }
        for (std::size_t j = 0; j < outputs.size(); ++j) {
            if (outputs[i].slots[1] == outputs[j].slots[0]
                || (i != j && outputs[i].slots[1] == outputs[j].slots[1])
                || (i % 3 != j % 3 && outputs[i].slots[0] == outputs[j].slots[0])) {
                return false;
            }
        }
    }
    return true;
}

bool multiplier(std::size_t tier,
                std::uint16_t socketType,
                std::uint16_t currency,
                std::uint32_t& output) noexcept {
    const std::lock_guard lock{g_costMutex};
    if (!g_costsReady || tier >= g_costs.size() || g_costs[tier].socketType != socketType) {
        return false;
    }
    const auto& cost = g_costs[tier];
    // An authored empty scalar list means the material set's base quantity.
    if (cost.count == 0) {
        output = 1;
        return true;
    }
    for (std::size_t i = 0; i < cost.count; ++i) {
        if (cost.scalars[i].itemIndex == currency) {
            output = cost.scalars[i].multiplier;
            return true;
        }
    }
    return false;
}

struct ExchangePair {
    items::Definition recipe{}, recycle{}, synth{}, mote{};
    items::details::Definition synthDetail{}, moteDetail{};
    materials::Definition synthesisCost{}, recycleCost{};
    std::uint16_t synthesisSocketType{};
};

bool single_cost(const items::Definition& plug,
                 std::uint16_t expectedItem,
                 materials::Definition& costs) noexcept {
    if (plug.insertionMaterialRequirementSetIndex == materials::kUnavailableSetIndex
        || !build_data::find_material_requirement_set(plug.insertionMaterialRequirementSetIndex,
                                                      costs)
        || costs.requirementSetIndex != plug.insertionMaterialRequirementSetIndex
        || costs.requirementCount != 1) {
        return false;
    }
    const auto& cost = costs.requirements[0];
    return cost.itemDefinitionIndex == expectedItem && cost.quantity > 0 && cost.deleteOnAction
           && !cost.omitFromRequirements && cost.condition == materials::kUnconditionalRequirement;
}

/** Validate both directions before either is usable. The paired tier's container supplies
 * the synthesis socket type even when a lower-tier held container recycles this Mote. */
bool resolve_pair(const Role& role, std::size_t tier, ExchangePair& pair) noexcept {
    items::Definition reference{};
    items::details::Definition referenceDetail{};
    if (tier >= kContainers.size()
        || !resolve_profile_stack(role.synth, pair.synth, pair.synthDetail)
        || !resolve_profile_stack(role.motes[tier], pair.mote, pair.moteDetail)
        || pair.synth.bucketId != pair.mote.bucketId || pair.moteDetail.maxStackSize != 1
        || !build_data::find_item_definition_hash(role.plugs[tier], pair.recipe)
        || pair.recipe.definitionHash != role.plugs[tier]
        || pair.recipe.plugCategoryHash != 472268526U
        || !build_data::find_item_definition_hash(role.recyclePlugs[tier], pair.recycle)
        || pair.recycle.definitionHash != role.recyclePlugs[tier]
        || pair.recycle.plugCategoryHash != 3057295571U
        || !single_cost(pair.recipe, pair.synth.definitionIndex, pair.synthesisCost)
        || !single_cost(pair.recycle, pair.mote.definitionIndex, pair.recycleCost)
        || pair.recycleCost.requirements[0].quantity != 1
        || !build_data::find_item_definition_hash(kContainers[tier], reference)
        || reference.definitionHash != kContainers[tier]
        || !build_data::find_configured_item_detail(reference.definitionIndex, referenceDetail)
        || referenceDetail.definitionIndex != reference.definitionIndex
        || referenceDetail.definitionHash != reference.definitionHash
        || referenceDetail.bucketId != reference.bucketId
        || referenceDetail.ordinarySocketState != items::details::OrdinarySocketState::present
        || referenceDetail.ordinarySocketCount != 5
        || referenceDetail.initialPlugIndices[tier] == items::details::kUnavailableItemIndex
        || referenceDetail.initialPlugIndices[4] == items::details::kUnavailableItemIndex
        || !build_data::is_socket_plug_allowed(
            reference.definitionIndex, static_cast<std::uint8_t>(tier), pair.recipe.definitionIndex)
        || !build_data::is_socket_plug_allowed(
            reference.definitionIndex, 4, pair.recycle.definitionIndex)) {
        return false;
    }
    pair.synthesisSocketType = referenceDetail.socketTypes[tier];
    std::uint32_t scalar{}, recycleScalar{};
    auto& cost = pair.synthesisCost.requirements[0];
    if (!multiplier(tier, pair.synthesisSocketType, pair.synth.definitionIndex, scalar)
        || !multiplier(3, referenceDetail.socketTypes[4], pair.mote.definitionIndex, recycleScalar)
        || recycleScalar != 1
        || cost.quantity
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) / scalar) {
        return false;
    }
    cost.quantity *= scalar;
    return true;
}

/** One private debit/credit after-image for both directions; no persistence or partial payment. */
bool exchange_profile(const AccountState& snapshot,
                      const materials::Definition& costs,
                      const items::details::Definition& reward,
                      std::int32_t quantity,
                      AccountState& candidate) noexcept {
    bool charged = false;
    if (quantity <= 0 || quantity > reward.maxStackSize
        || !apply_action_materials(snapshot, costs, candidate, charged) || !charged) {
        return false;
    }
    std::size_t rewardRows = 0;
    for (std::size_t i = 0; i < candidate.profileItemCount; ++i) {
        const auto& item = candidate.profileItems[i];
        if (item.definitionHash != reward.definitionHash) {
            continue;
        }
        // SocketPlug publishes one non-resident gain row. Never split or partly refund.
        if (++rewardRows != 1 || item.instanceSoid != 0
            || item.quantity > reward.maxStackSize - quantity) {
            return false;
        }
    }
    PendingProfileItemAcquisition grant{};
    // The direct shape supports a multi-unit refund against the charged view. Keep its
    // serial above the original view as well if payment removed the greatest serial.
    if (!finalize_profile_item_acquisition(candidate,
                                           candidate,
                                           reward.definitionHash,
                                           reward,
                                           false,
                                           quantity,
                                           {.direct = true},
                                           grant)
        || grant.acquiredInstanceSoid != 0
        || grant.afterItems[grant.profileIndex].instanceSoid != 0) {
        return false;
    }
    std::int32_t greatest = 0;
    for (std::size_t i = 0; i < snapshot.profileItemCount; ++i) {
        greatest = (std::max)(greatest, snapshot.profileItems[i].mutationSerial);
    }
    if (grant.acquiredMutationSerial <= greatest) {
        if (greatest == (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        grant.acquiredMutationSerial = greatest + 1;
        grant.afterItems[grant.profileIndex].mutationSerial = grant.acquiredMutationSerial;
    }
    candidate.profileItems = grant.afterItems;
    candidate.profileItemCount = grant.afterItemCount;
    return true;
}

std::size_t tier(std::uint32_t hash) noexcept {
    for (std::size_t i = 0; i < kContainers.size(); ++i) {
        if (hash == kContainers[i]) {
            return i + 1;
        }
    }
    return 0;
}

bool resolve_upgrade_container(std::uint32_t hash, items::details::Definition& detail) noexcept {
    items::Definition definition{};
    buckets::Descriptor bucket{};
    return build_data::find_item_definition_hash(hash, definition)
           && definition.definitionHash == hash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex && detail.definitionHash == hash
           && detail.bucketId == definition.bucketId
           && build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
           && bucket.arraySelector == buckets::ArraySelector::character
           && detail.instancedDefinitionState == items::details::InstancedDefinitionState::instanced
           && detail.maxStackSize == 1 && !detail.equipmentSlot.has_value()
           && detail.objectiveCount == 0 && detail.lifetimeSeconds == 0
           && detail.ordinarySocketState == items::details::OrdinarySocketState::present
           && detail.ordinarySocketCount > 0
           && detail.ordinarySocketCount <= inventory::kPlugCapacity;
}

/** Retain existing recipe choices; initialize only a previously undeclared socket lane. */
bool upgrade_sockets(const items::details::Definition& before,
                     const items::details::Definition& after,
                     inventory::Sockets& sockets) noexcept {
    if (before.ordinarySocketCount != after.ordinarySocketCount) {
        return false;
    }
    const bool authored = sockets.policy == inventory::SocketPolicy::authored;
    if ((!authored && sockets.policy != inventory::SocketPolicy::nativeDefaults)
        || (authored && sockets.plugCount != before.ordinarySocketCount)) {
        return false;
    }
    std::size_t opened = 0;
    for (std::size_t lane = 0; lane < before.ordinarySocketCount; ++lane) {
        const auto oldType = before.socketTypes[lane];
        const auto newType = after.socketTypes[lane];
        if (oldType == newType) {
            if (before.initialPlugIndices[lane] != after.initialPlugIndices[lane]) {
                return false;
            }
            if (authored && sockets.plugs[lane]) {
                items::Definition plug{};
                if (!build_data::find_item_definition_hash(*sockets.plugs[lane], plug)
                    || plug.definitionHash != *sockets.plugs[lane]
                    || (plug.definitionIndex != after.initialPlugIndices[lane]
                        && !build_data::is_socket_plug_allowed(after.definitionIndex,
                                                               static_cast<std::uint8_t>(lane),
                                                               plug.definitionIndex))) {
                    return false;
                }
            }
            continue;
        }
        if (oldType != items::details::kUnavailableSocketType
            || before.initialPlugIndices[lane] != items::details::kUnavailableItemIndex
            || newType == items::details::kUnavailableSocketType
            || after.initialPlugIndices[lane] == items::details::kUnavailableItemIndex
            || (authored && sockets.plugs[lane])) {
            return false;
        }
        items::Definition plug{};
        if (!build_data::find_item_definition_index(after.initialPlugIndices[lane], plug)
            || plug.definitionIndex != after.initialPlugIndices[lane]) {
            return false;
        }
        if (authored) {
            sockets.plugs[lane] = plug.definitionHash;
        }
        ++opened;
    }
    return opened == 1;
}
} // namespace

void configure_socket_costs(const crafting::SynthesizerCosts& costs) noexcept {
    const std::lock_guard lock{g_costMutex};
    g_costs = costs;
    g_costsReady = true;
}

bool is_container(std::uint32_t hash) noexcept {
    return std::find(kContainers.begin(), kContainers.end(), hash) != kContainers.end();
}

bool is_mote(std::uint32_t hash) noexcept {
    return std::any_of(kRoles.begin(), kRoles.end(), [hash](const auto& role) {
        return std::find(role.motes.begin(), role.motes.end(), hash) != role.motes.end();
    });
}

bool configure_mote_output_flags(std::span<const crafting::MoteOutput> outputs) noexcept {
    if (outputs.size() != g_moteOutputs.size()) {
        return false;
    }
    std::array<MoteOutputFlags, 12> decoded{};
    for (const auto& output : outputs) {
        std::size_t chosen = decoded.size();
        for (std::size_t role = 0; role < kRoles.size(); ++role) {
            for (std::size_t tier = 0; tier < 3; ++tier) {
                if (kRoles[role].motes[tier] == output.hash) {
                    chosen = role * 3 + tier;
                }
            }
        }
        if (chosen == decoded.size() || decoded[chosen].configured
            || output.slots[0] == output.slots[1]) {
            return false;
        }
        decoded[chosen] = {.slots = output.slots, .configured = true};
    }
    if (!output_flags_ready(decoded)) {
        return false;
    }
    const std::lock_guard lock{g_costMutex};
    g_moteOutputs = decoded;
    return true;
}

bool mote_output_flags_ready() noexcept {
    const std::lock_guard lock{g_costMutex};
    return output_flags_ready(g_moteOutputs);
}

std::uint16_t mote_ownership_mask(std::span<const account::inventory::ProfileItem> rows) noexcept {
    std::uint16_t mask = 0;
    for (const auto& item : rows) {
        if (item.instanceSoid != 0 || item.quantity <= 0) {
            continue;
        }
        for (std::size_t role = 0; role < kRoles.size(); ++role) {
            for (std::size_t tier = 0; tier < 3; ++tier) {
                if (item.definitionHash == kRoles[role].motes[tier]) {
                    mask |= static_cast<std::uint16_t>(1U << (role * 3 + tier));
                }
            }
        }
    }
    return mask;
}

std::uint16_t mote_publication_mask(std::span<const account::inventory::ProfileItem> rows,
                                    std::uint16_t previousMoteMask) noexcept {
    return static_cast<std::uint16_t>((previousMoteMask & 0x0FFFU) | mote_ownership_mask(rows));
}

bool mote_ownership_changed(std::span<const account::inventory::ProfileItem> before,
                            std::span<const account::inventory::ProfileItem> after) noexcept {
    return mote_ownership_mask(before) != mote_ownership_mask(after);
}

bool project_mote_output_flags(const AccountState& account,
                               Family5State& family,
                               std::uint16_t previousMoteMask) noexcept {
    if (!account::valid(account) || !valid_profile_inventory(account)
        || family.flagCount > family.flags.size() || family.valueCount > family.values.size()) {
        return false;
    }
    std::array<MoteOutputFlags, 12> outputs{};
    {
        const std::lock_guard lock{g_costMutex};
        outputs = g_moteOutputs;
    }
    if (!output_flags_ready(outputs)) {
        return false;
    }
    std::array<UnlockFlagOverride, 15> derived{};
    std::array<bool, 15> publish{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto hash = kRoles[i / 3].motes[i % 3];
        bool held = false;
        for (std::size_t p = 0; p < account.profileItemCount; ++p) {
            const auto& item = account.profileItems[p];
            if (item.definitionHash != hash) {
                continue;
            }
            items::Definition definition{};
            items::details::Definition detail{};
            if (item.instanceSoid != 0 || item.quantity != 1
                || !resolve_profile_stack(hash, definition, detail) || detail.maxStackSize != 1) {
                return false;
            }
            held = true;
        }
        for (const auto slot : outputs[i].slots) {
            std::size_t at = 0;
            while (at < count && derived[at].slot != slot) {
                ++at;
            }
            if (at == count) {
                if (count == derived.size()) {
                    return false;
                }
                // The native merge takes the first nonzero source at each slot: zero
                // means no override. Use explicit false (1) so recycling overrides
                // an older item-derived true (2), including shared tier ownership.
                derived[count++] = {slot, 1};
            }
            if (held) {
                derived[at].value = 2;
            }
            const auto flags = std::span(family.flags).first(family.flagCount);
            const bool alreadyProjected = std::any_of(
                flags.begin(), flags.end(), [slot](const auto& flag) { return flag.slot == slot; });
            publish[at] =
                publish[at] || held || alreadyProjected || (previousMoteMask & (1U << i)) != 0;
        }
    }
    if (count != derived.size()) {
        return false;
    }
    auto after = family;
    after.flags = {};
    after.flagCount = 0;
    for (std::size_t i = 0; i < family.flagCount; ++i) {
        const auto row = family.flags[i];
        const bool replaced = std::any_of(derived.begin(), derived.end(), [row](const auto& value) {
            return value.slot == row.slot;
        });
        if (!replaced) {
            after.flags[after.flagCount++] = row;
        }
    }
    const auto publishedCount =
        static_cast<std::size_t>(std::count(publish.begin(), publish.end(), true));
    if (publishedCount > after.flags.size() - after.flagCount) {
        return false;
    }
    // The client retains evaluated output flags after an override disappears.
    // Keep clears in every replacement, including a second projection of the same family.
    // The caller carries cumulative publication history: last ownership alone loses the
    // clears at the next exchange. Never-owned flags need not consume the 100-row bank.
    for (std::size_t i = 0; i < derived.size(); ++i) {
        if (publish[i]) {
            after.flags[after.flagCount++] = derived[i];
        }
    }
    family = after;
    return true;
}

bool stage_exchange(const AccountState& snapshot,
                    std::size_t characterIndex,
                    std::uint64_t targetInstanceSoid,
                    std::uint8_t socketLane,
                    std::uint16_t plugDefinitionIndex,
                    PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const bool recycling = socketLane == 4;
    if ((socketLane >= 3 && !recycling) || !account::valid(snapshot)
        || !valid_profile_inventory(snapshot) || characterIndex >= snapshot.characterCount
        || !snapshot.characters[characterIndex].selected) {
        return false;
    }
    const auto& character = snapshot.characters[characterIndex];
    CharacterItemLocation location{};
    if (!find_character_item_location(character, targetInstanceSoid, location)
        || location.equipped) {
        return false;
    }
    const auto* target = character_item_at(character, location);
    items::Definition container{}, requested{}, initial{};
    items::details::Definition detail{};
    buckets::Descriptor targetBucket{};
    if (!target || !is_container(target->definitionHash) || target->quantity != 1
        || !build_data::find_item_definition_hash(target->definitionHash, container)
        || container.definitionHash != target->definitionHash
        || !build_data::find_configured_item_detail(container.definitionIndex, detail)
        || detail.definitionHash != container.definitionHash
        || detail.definitionIndex != container.definitionIndex
        || detail.bucketId != container.bucketId
        || detail.instancedDefinitionState != items::details::InstancedDefinitionState::instanced
        || detail.maxStackSize != 1 || detail.equipmentSlot.has_value()
        || !build_data::find_inventory_bucket_descriptor(container.bucketId, targetBucket)
        || targetBucket.arraySelector != buckets::ArraySelector::character
        || detail.ordinarySocketState != items::details::OrdinarySocketState::present
        || detail.ordinarySocketCount != 5
        || detail.initialPlugIndices[socketLane] == items::details::kUnavailableItemIndex
        || !build_data::find_item_definition_index(plugDefinitionIndex, requested)
        || requested.definitionIndex != plugDefinitionIndex
        || !build_data::is_socket_plug_allowed(
            container.definitionIndex, socketLane, plugDefinitionIndex)
        || !build_data::find_item_definition_index(detail.initialPlugIndices[socketLane], initial)
        || initial.definitionIndex != detail.initialPlugIndices[socketLane]) {
        return false;
    }
    const Role* role = nullptr;
    std::size_t tier = 0;
    for (const auto& row : kRoles) {
        for (std::size_t i = 0; i < kContainers.size(); ++i) {
            if ((recycling ? row.recyclePlugs[i] : row.plugs[i]) != requested.definitionHash) {
                continue;
            }
            if (role || (!recycling && i != socketLane)) {
                return false;
            }
            role = &row;
            tier = i;
        }
    }
    if (!role) {
        return false;
    }

    ExchangePair pair{};
    std::uint32_t recycleScalar{};
    if (!resolve_pair(*role, tier, pair)
        || (!recycling && detail.socketTypes[socketLane] != pair.synthesisSocketType)
        || (recycling
            && (!multiplier(3, detail.socketTypes[4], pair.mote.definitionIndex, recycleScalar)
                || recycleScalar != 1))) {
        return false;
    }
    const auto& costs = recycling ? pair.recycleCost : pair.synthesisCost;
    const auto& reward = recycling ? pair.synthDetail : pair.moteDetail;
    // Explicit Sunrise reversible-exchange policy. Native recycle roll-set payout is NOT
    // decoded: refund exactly the paired native synthesis base cost times socket scalar.
    const auto quantity =
        recycling ? static_cast<std::int32_t>(pair.synthesisCost.requirements[0].quantity) : 1;
    if (!recycling) {
        buckets::Descriptor bucket{};
        if (!build_data::find_inventory_bucket_descriptor(pair.mote.bucketId, bucket)) {
            return false;
        }
        std::size_t occupied = 0;
        for (std::size_t i = 0; i < snapshot.profileItemCount; ++i) {
            const auto& item = snapshot.profileItems[i];
            // The native tier pool is shared by all four roles, not just this recipe's output.
            for (const auto& other : kRoles) {
                if (item.definitionHash == other.motes[tier]) {
                    return false;
                }
            }
            items::Definition definition{};
            if (!build_data::find_item_definition_hash(item.definitionHash, definition)) {
                return false;
            }
            if (definition.bucketId == pair.mote.bucketId) {
                ++occupied;
            }
        }
        // Native synthesis VAL151 < VAL152 requires a free slot before spending the Synth.
        if (occupied >= bucket.slotCount) {
            return false;
        }
    }
    auto sockets = target->sockets;
    if (!resolve_socket_choices(detail, sockets)) {
        return false;
    }
    sockets.plugs[socketLane] = initial.definitionHash;
    AccountState candidate = snapshot;
    if (!exchange_profile(snapshot, costs, reward, quantity, candidate)) {
        return false;
    }
    auto* changed = character_item_at(candidate.characters[characterIndex], location);
    if (!changed) {
        return false;
    }
    changed->sockets = sockets;
    if (mote_output_flags_ready()) {
        // Reserve a representable ownership after-image before spending any materials.
        InvestmentState preview{};
        if (!investment_snapshot(preview)
            || !project_mote_output_flags(
                candidate,
                preview.family5,
                mote_ownership_mask(
                    std::span(snapshot.profileItems).first(snapshot.profileItemCount)))) {
            return false;
        }
    }
    if (!finalize_socket_plug(snapshot,
                              candidate,
                              characterIndex,
                              location,
                              socketLane,
                              plugDefinitionIndex,
                              costs,
                              mutation)) {
        return false;
    }
    if (recycling) {
        core::log::writef(core::log::Channel::state,
                          core::log::Level::info,
                          "ev=synthesizer_exchange stage=prepared action=recycle "
                          "policy=reverse_native_synthesis_cost character=0x%llX "
                          "mote=%u debit=1 synth=%u refund=%d tier=%zu",
                          static_cast<unsigned long long>(character.soid),
                          pair.mote.definitionHash,
                          pair.synth.definitionHash,
                          quantity,
                          tier + 1);
    }
    return true;
}
bool stage_upgrade(CharacterState& character) noexcept {
    if (character.inventory.count > character.inventory.values.size()
        || character.gambitPrimeSynthesizerTier > GambitPrimeSynthesizerTier::powerful) {
        return false;
    }
    // These containers have no equipment slot. Refuse an ambiguous or malformed save instead
    // of moving equipment, selecting one duplicate, or granting a second container.
    for (const auto& item : character.equipment.slots) {
        if (item && tier(item->definitionHash) != 0) {
            return false;
        }
    }
    std::size_t heldIndex = character.inventory.count;
    std::size_t heldTier = 0;
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        const auto candidateTier = tier(character.inventory.values[i].definitionHash);
        if (candidateTier == 0) {
            continue;
        }
        if (heldTier != 0) {
            return false;
        }
        heldIndex = i;
        heldTier = candidateTier;
    }
    if (heldTier == 0) {
        return false;
    }
    const auto& held = character.inventory.values[heldIndex];
    items::details::Definition before{};
    if (held.instanceSoid == 0 || held.quantity != 1
        || !resolve_upgrade_container(held.definitionHash, before)
        || held.objectiveDefinitionIndex != items::details::kUnavailableItemIndex) {
        return false;
    }
    for (const auto value : held.objectiveValues) {
        if (value != 0) {
            return false;
        }
    }
    if (heldTier == kContainers.size()) {
        character.gambitPrimeSynthesizerTier = GambitPrimeSynthesizerTier::powerful;
        return true;
    }
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    items::details::Definition after{};
    auto replacement = held;
    if (!resolve_upgrade_container(kContainers[heldTier], after)
        || before.bucketId != after.bucketId
        || !upgrade_sockets(before, after, replacement.sockets)) {
        return false;
    }
    replacement.definitionHash = after.definitionHash;
    replacement.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial);
    // The held definition is authoritative: the previous reward implementation could advance
    // this selector without replacing the item. One redemption must open exactly one tier.
    character.inventory.values[heldIndex] = replacement;
    ++character.nextInventorySerial;
    character.gambitPrimeSynthesizerTier = static_cast<GambitPrimeSynthesizerTier>(heldTier + 1);
    return true;
}

} // namespace sunrise::state::runtime::detail::synthesizer
