#include "client_console_commands.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

#include "../../core/logging/log.h"
#include "../../core/runtime/wall_clock.h"
#include "../../server/bap/runtime.h"
#include "../../state/account/pursuit_hold.h"
#include "../../state/build_data/progressions/progression_catalog.h"
#include "../../state/build_data/pursuits/pursuit_progress.h"
#include "../../state/build_data/runtime.h"
#include "../../state/build_data/season_pass/season_pass_catalog.h"
#include "../../state/runtime/developer_investment_runtime.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_runtime.h"
#include "../movement/movement_settings_store.h"
#include "../player/player_position.h"
#include "../player/player_settings_store.h"
#include "console_registry.h"
#include "pursuit_grant_lists.h"

namespace sunrise::client::console {
namespace {

namespace data = state::build_data;
namespace inventory = state::account::inventory;
namespace unlocks = state::unlocks;

const state::CharacterState* selected(const state::AccountState& account) noexcept {
    for (std::size_t index = 0;
         index < (std::min)(account.characterCount, account.characters.size());
         ++index) {
        if (account.characters[index].selected) return &account.characters[index];
    }
    return nullptr;
}

std::unique_ptr<state::AccountState> account_view(Output& output) noexcept {
    std::unique_ptr<state::AccountState> account(
        new (std::nothrow) state::AccountState(state::account_snapshot()));
    if (!account) {
        output.line("Account snapshot allocation failed.");
        return nullptr;
    }
    if (!state::account::valid(*account)) {
        output.line("Account State unavailable or invalid; check investment database startup.");
        return nullptr;
    }
    return account;
}

bool resolve_item(std::uint16_t index,
                  data::items::Definition& item,
                  data::items::details::Definition& detail) noexcept {
    return data::find_item_definition_index(index, item)
           && data::find_configured_item_detail(index, detail)
           && detail.definitionHash == item.definitionHash && detail.bucketId == item.bucketId;
}

// Same structural rule as State's pursuit hold gate, limited to character inventory buckets.
bool pursuit(const data::items::details::Definition& detail) noexcept {
    data::inventory::buckets::Descriptor bucket{};
    return detail.objectiveCount != 0 && detail.objectiveCount <= detail.objectiveIndices.size()
           && !detail.equipmentSlot.has_value() && detail.maxStackSize <= 1
           && data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
           && bucket.arraySelector == data::inventory::buckets::ArraySelector::character;
}

bool item_held(std::span<const Value> arguments, Output& output) noexcept {
    const auto account = account_view(output);
    if (!account) return false;
    const auto* character = selected(*account);
    if (!character) {
        output.line("No character is selected.");
        return false;
    }
    if (!data::item_definitions_ready()) {
        output.line("Installed item definitions are not ready.");
        return false;
    }
    const bool all = !arguments.empty() && arguments[0].boolean;
    std::size_t shown = 0;
    std::size_t unresolved = 0;
    const auto show = [&](const inventory::Item& held, const char* location) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        const bool resolved = data::find_item_definition_hash(held.definitionHash, item)
                              && resolve_item(item.definitionIndex, item, detail);
        if (!resolved) ++unresolved;
        if (!all && (!resolved || !pursuit(detail))) return;
        output.format("  %s item=%d hash=0x%08X instance=0x%016llX quantity=%d%s",
                      location,
                      resolved ? static_cast<int>(item.definitionIndex) : -1,
                      held.definitionHash,
                      static_cast<unsigned long long>(held.instanceSoid),
                      held.quantity,
                      resolved ? "" : " (definition unavailable)");
        ++shown;
    };
    for (std::size_t i = 0;
         i < (std::min)(character->inventory.count, character->inventory.values.size());
         ++i) {
        show(character->inventory.values[i], "inventory");
    }
    if (all) {
        for (const auto& slot : character->equipment.slots)
            if (slot) show(*slot, "equipped");
        for (std::size_t i = 0;
             i < (std::min)(character->stacks.count, character->stacks.values.size());
             ++i) {
            const auto& stack = character->stacks.values[i];
            output.format(
                "  character stack hash=0x%08X quantity=%d", stack.definitionHash, stack.quantity);
            ++shown;
        }
    }
    output.format("%zu held rows shown; %zu item definitions unresolved.", shown, unresolved);
    return unresolved == 0;
}

bool report_mutation(const char* command,
                     const state::developer::Result& result,
                     Output& output) noexcept {
    // State returned: its transaction destructor has released SQLite before this callback.
    if (result.changed != 0) server::bap::request_account_resync();
    output.format("%s: %s; changed=%zu", command, result.reason, result.changed);
    return result.accepted;
}

bool item_grant(std::span<const Value> arguments, Output& output) noexcept {
    return report_mutation(
        "item.grant",
        state::developer::grant_item(
            static_cast<std::uint16_t>(arguments[0].integer),
            arguments.size() < 2 ? 1 : static_cast<std::int32_t>(arguments[1].integer)),
        output);
}

bool item_drop(std::span<const Value> arguments, Output& output) noexcept {
    return report_mutation(
        "item.drop",
        state::developer::drop_item(static_cast<std::uint16_t>(arguments[0].integer)),
        output);
}

bool pursuit_dropall(std::span<const Value>, Output& output) noexcept {
    return report_mutation("pursuit.dropall", state::developer::drop_pursuits(), output);
}

bool bounties_dropall(std::span<const Value>, Output& output) noexcept {
    return report_mutation("bounties.dropall", state::developer::drop_bounties(), output);
}

bool quest_set(std::span<const Value> arguments, Output& output) noexcept {
    const auto index = static_cast<std::uint16_t>(arguments[0].integer);
    if (arguments.size() == 4 && arguments[3].integer != index) {
        output.line("quest.set: tail definition must match the installed item; arbitrary remapping "
                    "is refused.");
        return false;
    }
    return report_mutation(
        "quest.set",
        state::developer::set_quest(
            index,
            static_cast<std::int32_t>(arguments[1].integer),
            arguments.size() < 3 ? 0 : static_cast<std::uint8_t>(arguments[2].integer)),
        output);
}

bool quest_objective(std::span<const Value> arguments, Output& output) noexcept {
    const auto index = static_cast<std::uint16_t>(arguments[0].integer);
    data::objectives::Definition objective{};
    if (!data::find_objective_definition(index, objective)) {
        output.format("quest.objective: no objective at %u", static_cast<unsigned>(index));
        return false;
    }
    output.format("objective %u hash=0x%08X completes=%d",
                  static_cast<unsigned>(index),
                  objective.definitionHash,
                  objective.completionValue);
    return true;
}

bool pursuit_display(std::span<const Value>, Output& output) noexcept {
    output.line("semantic reward rows are interpreted by policy and are never minted as items");
    return true;
}

bool pursuit_give_default(std::span<const Value>, Output& output) noexcept {
    // The original dirty/quest reward-policy set, resolved against the installed item catalog.
    constexpr std::array<std::uint16_t, 14> indices{14010,
                                                    14007,
                                                    14158,
                                                    14397,
                                                    14051,
                                                    14112,
                                                    14096,
                                                    14430,
                                                    14572,
                                                    14023,
                                                    14271,
                                                    14723,
                                                    14543,
                                                    14238};
    std::size_t changed = 0, accepted = 0;
    for (const auto index : indices) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!resolve_item(index, item, detail) || !pursuit(detail)) {
            output.format("  item=%u: installed pursuit unavailable", static_cast<unsigned>(index));
            continue;
        }
        const auto result = state::developer::grant_item(index, 1, item.definitionHash);
        output.format("  item=%u: %s", static_cast<unsigned>(index), result.reason);
        changed += result.changed;
        accepted += result.accepted;
    }
    if (changed != 0) server::bap::request_account_resync();
    output.format("pursuit.give: %zu/%zu granted or already held; %zu changed",
                  accepted,
                  indices.size(),
                  changed);
    return accepted == indices.size();
}

const char* bounty_page_mode(std::size_t index) noexcept {
    constexpr std::array<const char*, 3> modes{"complete", "give", nullptr};
    return index < modes.size() ? modes[index] : nullptr;
}

bool bounty_page(std::span<const Value> arguments, Output& output) noexcept {
    const bool giveOnly = arguments.size() > 1 && arguments[1].text == "give";
    if (arguments.size() > 1 && !giveOnly && arguments[1].text != "complete") {
        output.line("bounty.page: mode must be complete or give");
        return false;
    }
    constexpr std::size_t pageSize = 59; // dirty/quest's ordered bounty pages.
    const auto definitions = (std::min)(data::item_definition_count(), std::size_t{65536});
    const auto isBounty = [](std::uint16_t index,
                             data::items::Definition& item,
                             data::items::details::Definition& detail) noexcept {
        return resolve_item(index, item, detail) && pursuit(detail) && detail.bucketId == 40
               && detail.lifetimeSeconds > 0;
    };
    std::size_t total = 0;
    for (std::size_t i = 0; i < definitions; ++i) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        total += isBounty(static_cast<std::uint16_t>(i), item, detail);
    }
    const auto pages = (total + pageSize - 1) / pageSize;
    output.format(
        "bounty.page: %zu installed bounties; pages 1-%zu, %zu per page", total, pages, pageSize);
    if (arguments.empty()) {
        output.line(
            "bounty.page <page> [complete|give]: default completes; give preserves progress.");
        return total != 0;
    }
    const auto page = static_cast<std::size_t>(arguments[0].integer);
    if (page == 0 || page > pages) {
        output.line("Page is outside the installed bounty range.");
        return false;
    }
    const auto account = account_view(output);
    if (!account || !selected(*account)) {
        output.line("bounty.page: no selected character");
        return false;
    }
    const auto first = (page - 1) * pageSize;
    const auto* character = selected(*account);
    std::size_t ordinal = 0, changed = 0, completed = 0, refused = 0;
    std::size_t granted = 0, reused = 0, unchanged = 0;
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=bounty_page stage=begin page=%zu mode=%s held=%zu",
                      page,
                      giveOnly ? "give" : "complete",
                      character->inventory.count);
    for (std::size_t i = 0; i < definitions && ordinal < first + pageSize; ++i) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!isBounty(static_cast<std::uint16_t>(i), item, detail)) continue;
        if (ordinal++ < first) continue;
        bool held = false, heldComplete = true;
        for (std::size_t row = 0; row < character->inventory.count; ++row) {
            const auto& resident = character->inventory.values[row];
            if (resident.definitionHash != item.definitionHash) continue;
            held = true;
            heldComplete =
                heldComplete
                && data::pursuits::complete(item.definitionIndex, resident.objectiveValues);
        }
        if (held && (giveOnly || heldComplete)) {
            ++reused;
            ++unchanged;
            completed += !giveOnly;
            output.format("  item=%u: already held; unchanged",
                          static_cast<unsigned>(item.definitionIndex));
            continue;
        }
        const auto result =
            giveOnly ? state::developer::grant_item(item.definitionIndex, 1, item.definitionHash)
                     : state::developer::grant_complete_bounty(item.definitionIndex,
                                                               item.definitionHash);
        output.format("  item=%u: %s", static_cast<unsigned>(item.definitionIndex), result.reason);
        changed += result.changed;
        granted += result.accepted && !held;
        reused += held;
        completed += result.accepted && !giveOnly;
        refused += !result.accepted;
        if (!result.accepted)
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=bounty_page stage=refused page=%zu item=%u reason=%s",
                              page,
                              static_cast<unsigned>(item.definitionIndex),
                              result.reason);
    }
    // Publish once, after all per-bounty transactions release SQLite. No completion of other
    // held pursuits and no duplicate acquisition of an already-held page entry.
    if (changed != 0) server::bap::request_account_resync();
    output.format("bounty.page: page %zu/%zu; %zu granted, %zu reused, %zu completed, %zu "
                  "unchanged, %zu refused",
                  page,
                  pages,
                  granted,
                  reused,
                  completed,
                  unchanged,
                  refused);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=bounty_page stage=end page=%zu mode=%s granted=%zu reused=%zu "
                      "completed=%zu unchanged=%zu refused=%zu changed=%zu",
                      page,
                      giveOnly ? "give" : "complete",
                      granted,
                      reused,
                      completed,
                      unchanged,
                      refused,
                      changed);
    return refused == 0;
}

bool pursuit_complete(std::span<const Value>, Output& output) noexcept {
    return report_mutation("pursuit.complete", state::developer::complete_pursuits(), output);
}

template <unsigned Set> bool pursuit_give(std::span<const Value>, Output& output) noexcept {
    const auto entries = []() -> std::span<const grant_lists::Item> {
        if constexpr (Set == 2)
            return grant_lists::give2;
        else if constexpr (Set == 3)
            return grant_lists::give3;
        else if constexpr (Set == 4)
            return grant_lists::give4;
        else if constexpr (Set == 5)
            return grant_lists::give5;
        else
            return grant_lists::give6;
    }();
    std::size_t granted = 0, reused = 0, failed = 0;
    for (const auto& entry : entries) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!data::find_item_definition_hash(entry.hash, item)
            || !resolve_item(item.definitionIndex, item, detail) || !pursuit(detail)) {
            output.format("  hash=0x%08X: installed pursuit unavailable", entry.hash);
            ++failed;
            continue;
        }
        const auto result = state::developer::grant_item(item.definitionIndex, 1, entry.hash);
        output.format("  item=%u hash=0x%08X: %s",
                      static_cast<unsigned>(item.definitionIndex),
                      entry.hash,
                      result.reason);
        if (!result.accepted)
            ++failed;
        else if (result.changed != 0)
            ++granted;
        else
            ++reused;
    }
    if (granted != 0) server::bap::request_account_resync();
    output.format("pursuit.give%u: %zu granted, %zu already held, %zu refused. Prior grants remain "
                  "on partial failure.",
                  Set,
                  granted,
                  reused,
                  failed);
    output.line(
        "Grant only: held progress and expiry preserved; no completion or gameplay credit.");
    return failed == 0;
}

bool pursuit_list(std::span<const Value> arguments, Output& output) noexcept {
    if (!data::item_definitions_ready() || !data::objective_definitions_ready()) {
        output.line("pursuit.list: installed item definitions are not ready.");
        return false;
    }
    const auto sample = arguments.empty() ? 16U : static_cast<unsigned>(arguments[0].integer);
    std::size_t total = 0;
    std::size_t missing = 0;
    const auto count = (std::min)(data::item_definition_count(), data::items::kDefinitionCapacity);
    for (std::size_t index = 0; index < count; ++index) {
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!resolve_item(static_cast<std::uint16_t>(index), item, detail)) {
            ++missing;
            continue;
        }
        if (!pursuit(detail)) continue;
        if (total++ < sample) {
            output.format("  item=%u hash=0x%08X bucket=%u",
                          static_cast<unsigned>(index),
                          item.definitionHash,
                          static_cast<unsigned>(item.bucketId));
        }
    }
    output.format(
        "pursuit.list: %zu installed objective-bearing pursuits; %zu details unavailable.",
        total,
        missing);
    output.line("Installed objectives do not establish gameplay event support.");
    return true;
}

void show_progress(const inventory::Item& held,
                   std::uint16_t index,
                   const data::items::details::Definition& detail,
                   Output& output) noexcept {
    const auto progress = data::pursuits::measure(index, held.objectiveValues);
    const auto expiry = held.objectiveValues[inventory::kItemExpiryLane];
    const bool expired = expiry != 0 && expiry <= core::runtime::investment_clock_seconds();
    const bool tailMatches = held.objectiveDefinitionIndex == index;
    output.format("  item=%u instance=0x%016llX tail_definition=%u objectives=%u/%u resolved=%s "
                  "expiry=%d %s%s",
                  static_cast<unsigned>(index),
                  static_cast<unsigned long long>(held.instanceSoid),
                  static_cast<unsigned>(held.objectiveDefinitionIndex),
                  static_cast<unsigned>(progress.completeCount),
                  static_cast<unsigned>(progress.objectiveCount),
                  progress.resolved && tailMatches ? "yes" : "no",
                  expiry,
                  expired ? "expired" : "",
                  detail.lifetimeSeconds > 0 && expiry == 0 ? "missing expiry" : "");
    for (std::size_t i = 0; i < detail.objectiveCount; ++i) {
        data::objectives::Definition objective{};
        if (data::find_objective_definition(detail.objectiveIndices[i], objective))
            output.format("    lane=%zu objective=%u value=%d completion=%d",
                          i + 1,
                          static_cast<unsigned>(objective.definitionIndex),
                          held.objectiveValues[i + 1],
                          objective.completionValue);
        else
            output.format("    lane=%zu objective=%u metadata unavailable",
                          i + 1,
                          static_cast<unsigned>(detail.objectiveIndices[i]));
    }
}

bool pursuit_show(std::span<const Value> arguments, Output& output) noexcept {
    const auto index = static_cast<std::uint16_t>(arguments[0].integer);
    data::items::Definition item{};
    data::items::details::Definition detail{};
    if (!resolve_item(index, item, detail) || !pursuit(detail)) {
        output.line("pursuit.show: installed pursuit metadata unavailable.");
        return false;
    }
    output.format("pursuit.show: item=%u hash=0x%08X bucket=%u objectives=%u lifetime_seconds=%d "
                  "reward_rows=%u",
                  static_cast<unsigned>(index),
                  item.definitionHash,
                  static_cast<unsigned>(item.bucketId),
                  static_cast<unsigned>(detail.objectiveCount),
                  detail.lifetimeSeconds,
                  static_cast<unsigned>(detail.rewardCount));
    for (std::size_t i = 0; i < detail.rewardCount; ++i) {
        const auto& reward = detail.rewards[i];
        output.format("  reward item=%u companion=%u quantity=%d",
                      static_cast<unsigned>(reward.itemIndex),
                      static_cast<unsigned>(reward.companionIndex),
                      reward.quantity);
    }
    const auto account = account_view(output);
    const auto* character = account ? selected(*account) : nullptr;
    if (!character) {
        output.line("Held status unavailable: no selected character.");
        return false;
    }
    bool held = false;
    for (std::size_t i = 0; i < character->inventory.count; ++i) {
        if (character->inventory.values[i].definitionHash != item.definitionHash) continue;
        held = true;
        show_progress(character->inventory.values[i], index, detail, output);
    }
    if (!held) {
        output.line("Not held by selected character.");
        for (std::size_t i = 0; i < detail.objectiveCount; ++i) {
            data::objectives::Definition objective{};
            if (data::find_objective_definition(detail.objectiveIndices[i], objective))
                output.format("  lane=%zu objective=%u completion=%d",
                              i + 1,
                              static_cast<unsigned>(objective.definitionIndex),
                              objective.completionValue);
            else
                output.format("  lane=%zu objective metadata unavailable", i + 1);
        }
    }
    return true;
}

bool pursuit_status(std::span<const Value>, Output& output) noexcept {
    const auto account = account_view(output);
    const auto* character = account ? selected(*account) : nullptr;
    if (!character) {
        output.line("pursuit.status: no selected character.");
        return false;
    }
    std::size_t shown = 0, unresolved = 0;
    for (std::size_t i = 0; i < character->inventory.count; ++i) {
        const auto& held = character->inventory.values[i];
        data::items::Definition item{};
        data::items::details::Definition detail{};
        if (!data::find_item_definition_hash(held.definitionHash, item)
            || !resolve_item(item.definitionIndex, item, detail)) {
            ++unresolved;
            continue;
        }
        if (!pursuit(detail)) continue;
        show_progress(held, item.definitionIndex, detail, output);
        ++shown;
    }
    output.format(
        "pursuit.status: %zu held pursuits; %zu held item details unavailable.", shown, unresolved);
    return unresolved == 0;
}

const char* progression_scope(std::size_t index) noexcept {
    constexpr std::array<const char*, 2> names{"account", "character"};
    return index < names.size() ? names[index] : nullptr;
}

bool prog_show(std::span<const Value> arguments, Output& output) noexcept {
    if (arguments[0].text == "character") {
        const auto account = account_view(output);
        if (!account || !selected(*account)) {
            output.line(
                "prog.show: no selected character; refusing the upstream slot-zero fallback.");
            return false;
        }
    }
    std::unique_ptr<unlocks::Table> table(new (std::nothrow) unlocks::Table);
    if (!table || !unlocks::snapshot(*table)) {
        output.line("prog.show: State unlock snapshot unavailable.");
        return false;
    }
    const auto& bank =
        arguments[0].text == "account" ? table->accountProgressions : table->characterProgressions;
    std::size_t shown = 0;
    for (std::size_t i = 0; i < bank.size(); ++i) {
        if (bank[i] == unlocks::ProgressionLanes{}) continue;
        output.format("  definition=%zu lanes=%d,%d,%d", i, bank[i][0], bank[i][1], bank[i][2]);
        ++shown;
    }
    output.format("prog.show: %zu populated rows.", shown);
    return true;
}

bool prog_set(std::span<const Value> arguments, Output& output) noexcept {
    const auto scope = arguments[0].text == "account" ? data::progressions::Scope::account
                                                      : data::progressions::Scope::character;
    if (scope == data::progressions::Scope::character) {
        const auto account = account_view(output);
        if (!account || !selected(*account)) {
            output.line(
                "prog.set: no selected character; refusing the upstream slot-zero fallback.");
            return false;
        }
    }
    const auto index = static_cast<std::uint16_t>(arguments[1].integer);
    std::array<data::progressions::Definition, data::progressions::kDefinitionCapacity>
        definitions{};
    std::size_t count = 0;
    if (!data::progression_definitions_ready() || !data::progressions::snapshot(definitions, count)
        || index >= count || definitions[index].definitionIndex != index
        || definitions[index].scope != scope) {
        output.line("prog.set: definition is unavailable or belongs to another scope.");
        return false;
    }
    struct Change {
        bool account;
        std::size_t index;
        std::size_t lane;
        std::int32_t value;
    } change{scope == data::progressions::Scope::account,
             index,
             arguments.size() < 4 ? 0U : static_cast<std::size_t>(arguments[3].integer),
             static_cast<std::int32_t>(arguments[2].integer)};
    const bool saved = unlocks::mutate(&change, [](void* context, unlocks::Table& table) noexcept {
        const auto& value = *static_cast<const Change*>(context);
        auto& bank = value.account ? table.accountProgressions : table.characterProgressions;
        bank[value.index][value.lane] = value.value;
    });
    if (!saved) {
        output.line("prog.set: State transaction refused or database unavailable.");
        return false;
    }
    output.format("prog.set: definition=%u lane=%zu value=%d committed through State.",
                  static_cast<unsigned>(index),
                  change.lane,
                  change.value);
    server::bap::request_account_resync();
    output.line("Debug lane edit; derived season values are not recalculated by this command.");
    return true;
}

bool season_probe(std::span<const Value>, Output& output) noexcept {
    const auto account = account_view(output);
    std::unique_ptr<unlocks::Table> table(new (std::nothrow) unlocks::Table);
    if (!account || !table || !unlocks::snapshot(*table) || !data::season_pass_ready()) {
        output.line("season.probe: account, unlock banks or installed season catalog unavailable.");
        return false;
    }
    std::array<data::season_pass::Reward, data::season_pass::kRewardCapacity> rewards{};
    std::size_t count = 0;
    if (!data::season_pass::snapshot(rewards, count)) {
        output.line("season.probe: installed reward snapshot unavailable.");
        return false;
    }
    std::size_t claimed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto flag = rewards[i].claimFlagIndex;
        if (flag < table->accountFlags.size() && table->accountFlags[flag] == unlocks::kFlagSet)
            ++claimed;
    }
    output.format(
        "season: xp=%d rank=%u claimed_rows=%zu/%zu artifact_power=%u artifact_mask=0x%08X",
        state::seasonal_experience(),
        static_cast<unsigned>(state::seasonal_rank()),
        claimed,
        count,
        static_cast<unsigned>(state::artifact_power_bonus()),
        state::artifact_mod_mask());
    return true;
}

bool season_dump(std::span<const Value> arguments, Output& output) noexcept {
    std::array<data::season_pass::Reward, data::season_pass::kRewardCapacity> rewards{};
    std::size_t count = 0;
    std::unique_ptr<unlocks::Table> table(new (std::nothrow) unlocks::Table);
    if (!data::season_pass_ready() || !data::season_pass::snapshot(rewards, count) || !table
        || !unlocks::snapshot(*table)) {
        output.line("season.dump: installed reward catalog or State unlock snapshot unavailable.");
        return false;
    }
    const auto start = arguments.empty() ? 0U : static_cast<std::size_t>(arguments[0].integer);
    const auto limit = arguments.size() < 2 ? 16U : static_cast<std::size_t>(arguments[1].integer);
    if (start >= count) {
        output.format("season.dump: start row is outside %zu installed reward rows.", count);
        return false;
    }
    for (std::size_t row = start; row < (std::min)(count, start + limit); ++row) {
        const auto& reward = rewards[row];
        const char* claimed = reward.claimFlagIndex >= table->accountFlags.size() ? "unmapped"
                              : table->accountFlags[reward.claimFlagIndex] == unlocks::kFlagSet
                                  ? "yes"
                                  : "no";
        output.format("  row=%zu rank=%u item=%u hash=0x%08X quantity=%u claimed=%s",
                      row,
                      static_cast<unsigned>(reward.requiredRank),
                      static_cast<unsigned>(reward.itemIndex),
                      reward.itemHash,
                      reward.quantity,
                      claimed);
    }
    return true;
}

bool bounty_progress(std::span<const Value>, Output& output) noexcept {
    const auto status = server::bap::gameplay_investment_status();
    output.format("bounty.progress: decoded=%llu accepted=%llu refused=%llu duplicates=%llu "
                  "lanes=%llu triumphs=%llu",
                  static_cast<unsigned long long>(status.decoded),
                  static_cast<unsigned long long>(status.accepted),
                  static_cast<unsigned long long>(status.refused),
                  static_cast<unsigned long long>(status.duplicates),
                  static_cast<unsigned long long>(status.lanes),
                  static_cast<unsigned long long>(status.triumphs));
    output.format("  enemy_classes=%zu combat_labels=%zu last_reason=%s",
                  status.enemyClasses,
                  status.combatLabels,
                  status.lastReason != nullptr ? status.lastReason : "unavailable");
    output.line("Read-only production wire diagnostics; gameplay processing is automatic.");
    return true;
}

bool player_position(std::span<const Value>, Output& output) noexcept {
    const auto position = player::position::snapshot();
    if (!position.present) {
        output.line("player.position: the upstream position observer has no current sample.");
        return false;
    }
    output.format("player.position: %.3f %.3f %.3f",
                  static_cast<double>(position.position[0]),
                  static_cast<double>(position.position[1]),
                  static_cast<double>(position.position[2]));
    return true;
}

template <bool movement::Settings::* Member>
bool movement_toggle(std::span<const Value> arguments, Output& output) noexcept {
    auto settings = movement::get();
    if (!arguments.empty()) {
        settings.*Member = arguments[0].boolean;
        if (!movement::publish(settings)) {
            output.line("Movement settings update refused.");
            return false;
        }
    }
    output.format("%s (upstream movement setting)", settings.*Member ? "on" : "off");
    return true;
}

template <bool player::Settings::* Member>
bool player_toggle(std::span<const Value> arguments, Output& output) noexcept {
    auto settings = player::get();
    if (!arguments.empty()) {
        settings.*Member = arguments[0].boolean;
        if (!player::publish(settings)) {
            output.line("Player settings update refused.");
            return false;
        }
    }
    output.format("%s (upstream player setting)", settings.*Member ? "on" : "off");
    return true;
}

} // namespace

bool install_commands() noexcept {
    const Parameter item{
        "item", "Installed item definition index.", ValueType::integer, nullptr, 0, 65535};
    const Parameter toggle{
        "state", "on/off; omit to report.", ValueType::boolean, nullptr, 0, 0, true};
    const Parameter scope{"scope", "account or character.", ValueType::text, &progression_scope};
    const std::array entries{
        Entry{"item.held",
              "Lists held pursuits; all=on includes equipped items and character stacks.",
              &item_held,
              {Parameter{
                  "all", "Include all character items.", ValueType::boolean, nullptr, 0, 0, true}}},
        Entry{"item.grant",
              "Atomically grants installed items through State acquisition/reward policy.",
              &item_grant,
              {item,
               Parameter{"count",
                         "Quantity; default one. Pursuits require one.",
                         ValueType::integer,
                         nullptr,
                         1,
                         64,
                         true}}},
        Entry{"pursuit.list",
              "Lists installed objective-bearing pursuits, with a bounded sample.",
              &pursuit_list,
              {Parameter{"sample",
                         "Rows to print; default 16.",
                         ValueType::integer,
                         nullptr,
                         1,
                         256,
                         true}}},
        Entry{"pursuit.show",
              "Shows installed objectives, rewards, lifetime and held progress.",
              &pursuit_show,
              {item}},
        Entry{"pursuit.status",
              "Shows held objective progress and expiry from State.",
              &pursuit_status},
        Entry{"item.drop",
              "Removes all unequipped copies of an installed item without rewards.",
              &item_drop,
              {item}},
        Entry{"pursuit.dropall",
              "Removes held objective-bearing pursuits without rewards; preserves oven and reward "
              "banks.",
              &pursuit_dropall},
        Entry{"bounties.dropall",
              "Removes held bounties without rewards; preserves quests, oven and stacks.",
              &bounties_dropall},
        Entry{"pursuit.complete",
              "Completes held objectives atomically; no grants or redemption.",
              &pursuit_complete},
        Entry{"pursuit.give",
              "Grants the original 14-bounty reward-policy set.",
              &pursuit_give_default},
        Entry{"pursuit.display",
              "Reports the non-minting policy for semantic reward rows.",
              &pursuit_display,
              {Parameter{"legacy",
                         "Ignored compatibility toggle.",
                         ValueType::boolean,
                         nullptr,
                         0,
                         0,
                         true}}},
        Entry{"quest.objective",
              "Resolves one objective definition and completion value.",
              &quest_objective,
              {Parameter{"objective",
                         "Objective definition index.",
                         ValueType::integer,
                         nullptr,
                         0,
                         65535}}},
        Entry{"bounty.page",
              "Lists page bounds, or grants one ordered bounty page; complete is the default mode.",
              &bounty_page,
              {Parameter{"page",
                         "One-based page; omit to list the range.",
                         ValueType::integer,
                         nullptr,
                         1,
                         65535,
                         true},
               Parameter{"mode",
                         "complete or give (preserve progress).",
                         ValueType::text,
                         &bounty_page_mode,
                         0,
                         0,
                         true}}},
        Entry{"pursuit.give2",
              "Grants the 57-item legacy set; preserves held progress.",
              &pursuit_give<2>},
        Entry{"pursuit.give3", "Grants the 20-item weapon/element subset.", &pursuit_give<3>},
        Entry{"pursuit.give4", "Grants the 18-item ability/precision subset.", &pursuit_give<4>},
        Entry{"pursuit.give5", "Grants 46 additions from legacy rows 57..107.", &pursuit_give<5>},
        Entry{"pursuit.give6", "Grants 41 additions from legacy rows 108..156.", &pursuit_give<6>},
        Entry{"quest.set",
              "Edits held progress; omitted lane means all objectives. Expiry is preserved.",
              &quest_set,
              {item,
               Parameter{"value",
                         "Signed progress value.",
                         ValueType::integer,
                         nullptr,
                         -2147483648.0,
                         2147483647.0},
               Parameter{"lane",
                         "Objective lane 1..7; omit for all.",
                         ValueType::integer,
                         nullptr,
                         1,
                         7,
                         true},
               Parameter{"definition",
                         "Compatibility argument; must equal item.",
                         ValueType::integer,
                         nullptr,
                         0,
                         65535,
                         true}}},
        Entry{"prog.show", "Lists populated persisted progression lanes.", &prog_show, {scope}},
        Entry{
            "prog.set",
            "Debug edit of one installed progression lane through State.",
            &prog_set,
            {scope,
             Parameter{
                 "definition", "Installed progression index.", ValueType::integer, nullptr, 0, 255},
             Parameter{"value",
                       "Signed 32-bit value.",
                       ValueType::integer,
                       nullptr,
                       -2147483648.0,
                       2147483647.0},
             Parameter{
                 "lane", "Lane 0-2; default zero.", ValueType::integer, nullptr, 0, 2, true}}},
        Entry{"season.probe",
              "Reads persisted season XP, rank, reward claims and artifact state.",
              &season_probe},
        Entry{
            "season.dump",
            "Lists installed season rewards and saved claim flags.",
            &season_dump,
            {Parameter{"start",
                       "First reward row; default zero.",
                       ValueType::integer,
                       nullptr,
                       0,
                       65535,
                       true},
             Parameter{
                 "count", "Rows to print; default 16.", ValueType::integer, nullptr, 1, 64, true}}},

        Entry{"bounty.progress",
              "Reads production gameplay counters and the last processing reason.",
              &bounty_progress},
        Entry{"player.position", "Reads the upstream position snapshot.", &player_position},
        Entry{"movement.fly",
              "Controls the upstream fly setting.",
              &movement_toggle<&movement::Settings::flyEnabled>,
              {toggle}},
        Entry{"movement.noclip",
              "Controls the upstream noclip setting.",
              &movement_toggle<&movement::Settings::noclipEnabled>,
              {toggle}},
        Entry{"movement.teleport",
              "Controls the upstream teleport setting.",
              &movement_toggle<&movement::Settings::enabled>,
              {toggle}},
        Entry{"player.infinite_ammo",
              "Controls the upstream infinite-ammo setting.",
              &player_toggle<&player::Settings::infiniteAmmoEnabled>,
              {toggle}},
        Entry{"inactivity.disable",
              "Controls the upstream anti-AFK setting.",
              &player_toggle<&player::Settings::antiAfkEnabled>,
              {toggle}},
    };
    for (const auto& entry : entries)
        if (!add(entry)) return false;
    return true;
}

} // namespace sunrise::client::console
