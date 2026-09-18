#include "synthesizer_upgrade_runtime.h"

#include <array>
#include <limits>

#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail::synthesizer {
namespace {
namespace inventory = account::inventory;
namespace items = build_data::items;
namespace buckets = build_data::inventory::buckets;

/** Reward policy identities only; indices, bucket, socket types and plugs are package data. */
constexpr std::array<std::uint32_t, 3> kTierHashes{1160544509U, 1160544508U, 1160544511U};

std::size_t tier(std::uint32_t hash) noexcept {
    for (std::size_t i = 0; i < kTierHashes.size(); ++i)
        if (hash == kTierHashes[i]) return i + 1;
    return 0;
}

bool resolve(std::uint32_t hash, items::details::Definition& detail) noexcept {
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
    if (before.ordinarySocketCount != after.ordinarySocketCount) return false;
    const bool authored = sockets.policy == inventory::SocketPolicy::authored;
    if ((!authored && sockets.policy != inventory::SocketPolicy::nativeDefaults)
        || (authored && sockets.plugCount != before.ordinarySocketCount))
        return false;
    std::size_t opened = 0;
    for (std::size_t lane = 0; lane < before.ordinarySocketCount; ++lane) {
        const auto oldType = before.socketTypes[lane];
        const auto newType = after.socketTypes[lane];
        if (oldType == newType) {
            if (before.initialPlugIndices[lane] != after.initialPlugIndices[lane]) return false;
            if (authored && sockets.plugs[lane]) {
                items::Definition plug{};
                if (!build_data::find_item_definition_hash(*sockets.plugs[lane], plug)
                    || plug.definitionHash != *sockets.plugs[lane]
                    || (plug.definitionIndex != after.initialPlugIndices[lane]
                        && !build_data::is_socket_plug_allowed(after.definitionIndex,
                                                               static_cast<std::uint8_t>(lane),
                                                               plug.definitionIndex)))
                    return false;
            }
            continue;
        }
        if (oldType != items::details::kUnavailableSocketType
            || before.initialPlugIndices[lane] != items::details::kUnavailableItemIndex
            || newType == items::details::kUnavailableSocketType
            || after.initialPlugIndices[lane] == items::details::kUnavailableItemIndex
            || (authored && sockets.plugs[lane]))
            return false;
        items::Definition plug{};
        if (!build_data::find_item_definition_index(after.initialPlugIndices[lane], plug)
            || plug.definitionIndex != after.initialPlugIndices[lane])
            return false;
        if (authored) sockets.plugs[lane] = plug.definitionHash;
        ++opened;
    }
    return opened == 1;
}
} // namespace

bool stage_upgrade(CharacterState& character) noexcept {
    if (character.inventory.count > character.inventory.values.size()
        || character.gambitPrimeSynthesizerTier > GambitPrimeSynthesizerTier::powerful)
        return false;
    // These containers have no equipment slot. Refuse an ambiguous or malformed save instead
    // of moving equipment, selecting one duplicate, or granting a second container.
    for (const auto& item : character.equipment.slots)
        if (item && tier(item->definitionHash) != 0) return false;
    std::size_t heldIndex = character.inventory.count;
    std::size_t heldTier = 0;
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        const auto candidateTier = tier(character.inventory.values[i].definitionHash);
        if (candidateTier == 0) continue;
        if (heldTier != 0) return false;
        heldIndex = i;
        heldTier = candidateTier;
    }
    if (heldTier == 0) return false;
    const auto& held = character.inventory.values[heldIndex];
    items::details::Definition before{};
    if (held.instanceSoid == 0 || held.quantity != 1 || !resolve(held.definitionHash, before)
        || held.objectiveDefinitionIndex != items::details::kUnavailableItemIndex)
        return false;
    for (const auto value : held.objectiveValues)
        if (value != 0) return false;
    if (heldTier == kTierHashes.size()) {
        character.gambitPrimeSynthesizerTier = GambitPrimeSynthesizerTier::powerful;
        return true;
    }
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
        return false;
    items::details::Definition after{};
    auto replacement = held;
    if (!resolve(kTierHashes[heldTier], after) || before.bucketId != after.bucketId
        || !upgrade_sockets(before, after, replacement.sockets))
        return false;
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
