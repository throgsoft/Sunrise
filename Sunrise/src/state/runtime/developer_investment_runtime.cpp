#include "developer_investment_runtime.h"

#include <array>
#include <limits>
#include <memory>
#include <new>

#include "../../core/runtime/wall_clock.h"
#include "../account/pursuit_hold.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"

namespace sunrise::state::developer {
namespace {
namespace data = build_data;
namespace inventory = account::inventory;
namespace store = investment::store;

CharacterState* selected(AccountState& account) noexcept {
    for (std::size_t i = 0; i < account.characterCount; ++i)
        if (account.characters[i].selected) return &account.characters[i];
    return nullptr;
}

bool resolve(std::uint16_t index,
             data::items::Definition& item,
             data::items::details::Definition& detail) noexcept {
    return data::find_item_definition_index(index, item)
           && data::find_configured_item_detail(index, detail)
           && item.definitionHash == detail.definitionHash && item.bucketId == detail.bucketId;
}

bool objectives(const data::items::details::Definition& detail,
                std::array<std::int32_t, inventory::kItemObjectiveLaneCount>& thresholds) noexcept {
    if (detail.objectiveCount == 0 || detail.objectiveCount > thresholds.size()) return false;
    for (std::size_t i = 0; i < detail.objectiveCount; ++i) {
        data::objectives::Definition objective{};
        if (!data::find_objective_definition(detail.objectiveIndices[i], objective)
            || objective.completionValue <= 0)
            return false;
        thresholds[i] = objective.completionValue;
    }
    return true;
}

const char* editable(const inventory::Item& held,
                     std::uint16_t index,
                     const data::items::details::Definition& detail) noexcept {
    if (held.objectiveDefinitionIndex != index && held.objectiveDefinitionIndex != 0xFFFFU)
        return "held objective definition differs from installed item";
    const auto expiry = held.objectiveValues[inventory::kItemExpiryLane];
    if (detail.lifetimeSeconds > 0 && expiry == 0) return "held timed pursuit has no saved expiry";
    if (expiry != 0 && expiry <= core::runtime::investment_clock_seconds())
        return "held pursuit has expired";
    return nullptr;
}

Result edit(std::uint16_t index, std::int32_t value, std::uint8_t lane, bool completeAll) noexcept {
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    if (!account) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*account) || !account::valid(*account))
        return {false, 0, "investment database unavailable"};
    auto* character = selected(*account);
    if (!character) return {false, 0, "no selected character"};
    data::items::Definition target{};
    if (!completeAll && !data::find_item_definition_index(index, target))
        return {false, 0, "installed item identity unavailable"};
    std::size_t matched = 0, changed = 0;
    for (std::size_t i = 0; i < character->inventory.count; ++i) {
        auto& held = character->inventory.values[i];
        if (!completeAll && held.definitionHash != target.definitionHash) continue;
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!data::find_item_definition_hash(held.definitionHash, item)
            || !resolve(item.definitionIndex, item, detail))
            return {false, 0, "held item metadata unavailable; no changes committed"};
        if (!completeAll && item.definitionIndex != index) continue;
        if (completeAll && detail.objectiveCount == 0) continue;
        std::array<std::int32_t, inventory::kItemObjectiveLaneCount> thresholds{};
        if (!objectives(detail, thresholds) || lane > detail.objectiveCount)
            return {false, 0, "objective metadata or lane unavailable; no changes committed"};
        if (const auto* reason = editable(held, item.definitionIndex, detail))
            return {false, 0, reason};
        ++matched;
        auto after = held.objectiveValues;
        for (std::size_t ordinal = 0; ordinal < detail.objectiveCount; ++ordinal) {
            const auto targetLane = ordinal + inventory::kItemObjectiveLaneBase;
            if (lane == 0 || lane == targetLane)
                after[targetLane] = completeAll ? thresholds[ordinal] : value;
        }
        if (after == held.objectiveValues && held.objectiveDefinitionIndex == item.definitionIndex)
            continue;
        constexpr auto maximum =
            static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
        if (character->nextInventorySerial >= maximum)
            return {false, 0, "inventory serial exhausted; no changes committed"};
        held.objectiveValues = after;
        held.objectiveDefinitionIndex = item.definitionIndex;
        held.mutationSerial = static_cast<std::int32_t>(character->nextInventorySerial++);
        ++changed;
    }
    if (matched == 0) return {false, 0, "no matching held pursuit on selected character"};
    if (changed != 0 && !store::write_account(*account)) return {false, 0, "account write refused"};
    if (!transaction.commit()) return {false, 0, "transaction commit failed"};
    return {true,
            changed,
            changed == 0 ? "already at requested values"
                         : "objective values committed; no rewards claimed"};
}
Result drop(std::uint16_t index, bool allPursuits) noexcept {
    data::items::Definition requested{};
    if (!allPursuits && !data::find_item_definition_index(index, requested))
        return {false, 0, "installed item identity unavailable"};
    std::unique_ptr<AccountState> snapshot(new (std::nothrow) AccountState);
    if (!snapshot) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*snapshot) || !account::valid(*snapshot))
        return {false, 0, "investment database unavailable"};
    auto* character = selected(*snapshot);
    if (!character) return {false, 0, "no selected character"};
    std::size_t kept = 0, removed = 0;
    const auto beforeCount = character->inventory.count;
    for (std::size_t i = 0; i < beforeCount; ++i) {
        const auto held = character->inventory.values[i];
        bool remove = held.definitionHash == requested.definitionHash;
        if (allPursuits) {
            data::items::Definition item{};
            data::items::details::Definition detail{};
            if (!data::find_item_definition_hash(held.definitionHash, item)
                || !resolve(item.definitionIndex, item, detail))
                return {false,
                        0,
                        "held metadata unavailable; use item.drop for a known identity; no changes "
                        "committed"};
            remove = detail.objectiveCount != 0 && !detail.equipmentSlot.has_value();
        }
        if (remove)
            ++removed;
        else
            character->inventory.values[kept++] = held;
    }
    if (removed == 0) return {true, 0, "no matching unequipped residents"};
    // Compact authored storage without inventing new identities or resetting survivor progress.
    for (std::size_t i = kept; i < beforeCount; ++i)
        character->inventory.values[i] = {};
    character->inventory.count = kept;
    if (!store::write_account(*snapshot) || !transaction.commit())
        return {false, 0, "removal transaction failed; no changes committed"};
    return {true, removed, "unequipped residents removed; no rewards, claims or refunds"};
}

} // namespace

Result grant_item(std::uint16_t index, std::int32_t quantity, std::uint32_t expectedHash) noexcept {
    if (quantity < 1 || quantity > 64) return {false, 0, "quantity must be 1..64"};
    data::items::Definition item{};
    data::items::details::Definition detail{};
    if (!resolve(index, item, detail) || (expectedHash != 0 && item.definitionHash != expectedHash))
        return {false, 0, "installed item identity/detail unavailable or changed"};
    std::unique_ptr<AccountState> account(new (std::nothrow) AccountState);
    std::unique_ptr<PendingRecordRewardGrant> pending(new (std::nothrow) PendingRecordRewardGrant);
    if (!account || !pending) return {false, 0, "allocation failed"};
    store::Transaction transaction;
    if (!transaction.ready() || !store::read_account(*account) || !account::valid(*account))
        return {false, 0, "investment database unavailable"};
    auto* character = selected(*account);
    if (!character) return {false, 0, "no selected character"};
    if (detail.objectiveCount != 0) {
        std::array<std::int32_t, inventory::kItemObjectiveLaneCount> thresholds{};
        if (!objectives(detail, thresholds)) return {false, 0, "objective thresholds unavailable"};
        if (quantity != 1) return {false, 0, "pursuits require quantity one"};
        for (std::size_t i = 0; i < character->inventory.count; ++i) {
            const auto& held = character->inventory.values[i];
            if (held.definitionHash != item.definitionHash) continue;
            if (const auto* reason = editable(held, index, detail)) return {false, 0, reason};
            return {true, 0, "already held; expiry and progress preserved"};
        }
    }
    const bool instanced = detail.instancedDefinitionState
                           == data::items::details::InstancedDefinitionState::instanced;
    const auto requests = instanced ? quantity : 1;
    const std::array rewards{DirectRecordReward{index, instanced ? 1 : quantity}};
    for (std::int32_t request = 0; request < requests; ++request) {
        if (!prepare_record_reward_grant(rewards, kUnclaimedRecordIndex, *pending))
            return {false,
                    0,
                    "State grant policy refused (item support, ownership, capacity or quantity); "
                    "request rolled back"};
        if (!commit_record_reward(*pending))
            return {false, 0, "State grant commit failed; request rolled back"};
    }
    if (!transaction.commit()) return {false, 0, "transaction commit failed; request rolled back"};
    return {true,
            static_cast<std::size_t>(requests),
            "grant committed through State acquisition/reward policy"};
}

Result set_quest(std::uint16_t index, std::int32_t value, std::uint8_t lane) noexcept {
    if (lane > inventory::kItemObjectiveLaneCount) return {false, 0, "objective lane must be 1..7"};
    return edit(index, value, lane, false);
}

Result complete_pursuits() noexcept {
    return edit(0, 0, 0, true);
}
Result drop_item(std::uint16_t index) noexcept {
    return drop(index, false);
}
Result drop_pursuits() noexcept {
    return drop(0, true);
}

} // namespace sunrise::state::developer
