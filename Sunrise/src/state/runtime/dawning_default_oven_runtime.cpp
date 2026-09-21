#include "dawning_default_oven_runtime.h"

#include <array>
#include <cstdio>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "../account/inventory/dawning_oven_state.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {
using Status = DefaultInventoryBootstrapStatus;

constexpr std::uint32_t kWeakSynthesizerHash = 1160544509U;
constexpr std::uint32_t kChaliceHash = 1115550924U;
constexpr std::array<std::uint32_t, 3> kSynthesizerHashes{
    kWeakSynthesizerHash, 1160544508U, 1160544511U};

/** Stable grant identities are policy; item indices, buckets and sockets come from the PKGs. */
DefaultInventoryBootstrapResult
ensure_default_item(std::uint32_t definitionHash,
                    const char* markerPrefix,
                    std::uint8_t socketCount,
                    std::span<const std::uint32_t> alternatives = {}) noexcept {
    namespace store = investment::store;
    namespace items = build_data::items;
    using namespace runtime::detail;
    items::Definition definition{};
    items::details::Definition detail{};
    build_data::inventory::buckets::Descriptor bucket{};
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::inventory_bucket_descriptors_ready()
        || !build_data::socket_entry_lists_ready())
        return {Status::notReady, false};
    if (!build_data::find_item_definition_hash(definitionHash, definition)
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
        || definition.definitionHash != definitionHash
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::character
        || detail.maxStackSize != 1 || detail.ordinarySocketCount != socketCount
        || detail.ordinarySocketState != items::details::OrdinarySocketState::present
        || detail.instancedDefinitionState != items::details::InstancedDefinitionState::instanced
        || detail.equipmentSlot.has_value())
        return {Status::refused, false};
    auto snapshot = std::unique_ptr<AccountState>{new (std::nothrow) AccountState};
    if (!snapshot) return {Status::refused, false};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*snapshot) || !account::valid(*snapshot))
        return {Status::refused, false};
    if (snapshot->characterCount == 0) return {Status::notReady, false};
    const auto originallySelected = selected_character_index(*snapshot);
    bool changed = false;
    // The initial Family-4 snapshot precedes character selection. Seed every character here,
    // otherwise a later preflight can persist an item without publishing its new object.
    for (std::size_t characterIndex = 0; characterIndex < snapshot->characterCount;
         ++characterIndex) {
        auto& character = snapshot->characters[characterIndex];
        std::array<char, 80> key{};
        const auto length = std::snprintf(key.data(),
                                          key.size(),
                                          "%s-%016llx",
                                          markerPrefix,
                                          static_cast<unsigned long long>(character.soid));
        if (length <= 0 || static_cast<std::size_t>(length) >= key.size())
            return {Status::refused, false};
        const std::string_view marker(key.data(), static_cast<std::size_t>(length));
        if (store::bootstrap_completed(marker)) continue;
        bool held = false;
        auto heldTier = GambitPrimeSynthesizerTier::none;
        const auto matches = [&](std::uint32_t hash) noexcept {
            if (definitionHash == kWeakSynthesizerHash) {
                for (std::size_t tier = 0; tier < kSynthesizerHashes.size(); ++tier) {
                    const auto value = static_cast<GambitPrimeSynthesizerTier>(tier + 1);
                    if (hash == kSynthesizerHashes[tier] && value > heldTier) heldTier = value;
                }
            }
            if (hash == definitionHash) return true;
            for (const auto alternative : alternatives)
                if (hash == alternative) return true;
            return false;
        };
        for (std::size_t i = 0; i < character.inventory.count; ++i)
            if (matches(character.inventory.values[i].definitionHash)) held = true;
        for (const auto& item : character.equipment.slots)
            if (item && matches(item->definitionHash)) held = true;
        if (!held) {
            auto pending =
                std::unique_ptr<PendingItemAcquisition>{new (std::nothrow) PendingItemAcquisition};
            // Select only in this private staging view, so the common acquisition validator can
            // resolve the target character. Never publish a synthetic character selection.
            for (std::size_t i = 0; i < snapshot->characterCount; ++i)
                snapshot->characters[i].selected = i == characterIndex;
            if (!pending
                || !finalize_item_acquisition(
                    *snapshot, *snapshot, definitionHash, false, {.direct = true}, *pending))
                return {Status::refused, false};
            character = pending->afterCharacter;
            if (definitionHash == kWeakSynthesizerHash) heldTier = GambitPrimeSynthesizerTier::weak;
            for (std::size_t i = 0; i < snapshot->characterCount; ++i)
                snapshot->characters[i].selected = i == originallySelected;
            changed = true;
        }
        // Prime's existing reward selector needs a starting tier. Derive a missing selector
        // only from actual ownership; preserve every already-earned selector and all sockets.
        if (heldTier != GambitPrimeSynthesizerTier::none
            && character.gambitPrimeSynthesizerTier == GambitPrimeSynthesizerTier::none) {
            character.gambitPrimeSynthesizerTier = heldTier;
            changed = true;
        }
        if (!store::complete_bootstrap(marker)) return {Status::refused, false};
    }
    if ((changed && !store::write_account(*snapshot)) || !transaction.commit())
        return {Status::refused, false};
    return {Status::ready, changed};
}
} // namespace

DefaultInventoryBootstrapResult ensure_default_dawning_oven() noexcept {
    return ensure_default_item(account::inventory::dawning::kOvenHash, "dawning-default-oven-v1", 5);
}

DefaultInventoryBootstrapResult ensure_default_activity_containers() noexcept {
    // Separate transactions let an unavailable item or full bucket leave the other grant usable.
    const auto synthesizer = ensure_default_item(kWeakSynthesizerHash,
                                                  "default-weak-synthesizer-v1",
                                                  5,
                                                  kSynthesizerHashes);
    const auto chalice = ensure_default_item(kChaliceHash, "default-chalice-of-opulence-v1", 8);
    auto status = Status::ready;
    if (synthesizer.status == Status::notReady || chalice.status == Status::notReady)
        status = Status::notReady;
    if (synthesizer.status == Status::refused || chalice.status == Status::refused)
        status = Status::refused;
    return {status, synthesizer.changed || chalice.changed};
}
} // namespace sunrise::state
