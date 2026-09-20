#include "chalice_crafting_runtime.h"

#include <algorithm>
#include <limits>
#include <mutex>

#include "../../core/logging/log.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::chalice {
namespace {
namespace items = build_data::items;
namespace materials = build_data::material_requirements;
namespace inventory = account::inventory;
namespace buckets = build_data::inventory::buckets;
namespace crafting = build_data::crafting;
using Plug = crafting::ChalicePlug;
using Expression = crafting::Expression;
namespace store = investment::store;

constexpr std::uint16_t kChaliceIndex = crafting::kChaliceIndex;
constexpr std::uint16_t kQuestValue = crafting::kQuestValue;
constexpr std::uint32_t kImperialsHash = 1642918584U;
constexpr auto& kFlagSlots = crafting::kChaliceFlagSlots;
constexpr auto& kSocketTypes = crafting::kChaliceSocketTypes;
std::mutex g_metadataMutex;
std::array<Plug, crafting::kChalicePlugCapacity> g_plugs{};
bool g_metadataReady{};

bool valid_state(const State& state) noexcept {
    return state.questProgress >= -1
           && std::all_of(state.runes.begin(), state.runes.end(), [](auto v) { return v >= 0; })
           && std::all_of(
               state.upgrades.begin(), state.upgrades.end(), [](auto v) { return v <= 2; });
}

bool read_state(State& state) noexcept {
    const std::lock_guard lock{store::g_mutex};
    state = {};
    for (std::size_t i = 0; i < state.runes.size(); ++i) {
        if (!store::read_unlock(store::Bank::objectiveValues,
                                static_cast<std::uint16_t>(kRuneValueBase + i),
                                state.runes[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < state.upgrades.size(); ++i) {
        std::int32_t flag{};
        if (!store::read_unlock(
                store::Bank::accountFlags, static_cast<std::uint16_t>(kUpgradeFlagBase + i), flag)
            || flag < 0 || flag > 2) {
            return false;
        }
        state.upgrades[i] = static_cast<std::uint8_t>(flag);
    }
    return store::read_unlock(store::Bank::objectiveValues, kQuestValue, state.questProgress)
           && valid_state(state);
}

bool resolve_plug(std::uint16_t index, items::Definition& definition, Plug& metadata) noexcept {
    if (!crafting::needs_chalice_item(index) || index == kChaliceIndex) {
        return false;
    }
    {
        const std::lock_guard lock{g_metadataMutex};
        metadata = g_plugs[index - kChaliceIndex];
    }
    return metadata.ready && build_data::find_item_definition_index(index, definition)
           && definition.definitionIndex == index && definition.definitionHash == metadata.hash;
}

int rune_index(std::uint16_t plug, std::size_t lane) noexcept {
    // Each of the three rune sockets has its own contiguous set of twelve plug definitions.
    const auto first = 7949 + lane * 12;
    return plug >= first && plug < first + 12 ? static_cast<int>(plug - first) : -1;
}

bool installed_rune(const inventory::Sockets& sockets, std::size_t lane, int& rune) noexcept {
    rune = -1;
    if (lane >= 3 || !sockets.plugs[4 + lane]) {
        return false;
    }
    items::Definition plug{};
    Plug metadata{};
    if (!build_data::find_item_definition_hash(*sockets.plugs[4 + lane], plug)
        || !resolve_plug(plug.definitionIndex, plug, metadata)) {
        return false;
    }
    rune = rune_index(plug.definitionIndex, lane);
    return rune >= 0 || plug.definitionIndex == 7938 + lane;
}

/** Only the decoded Chalice expression subset is admitted. Unknown sources fail closed. */
bool evaluate(const Expression& expr,
              const State& state,
              const inventory::Sockets& sockets,
              bool& result) noexcept {
    std::array<std::int32_t, 8> stack{};
    std::size_t size = 0;
    for (std::size_t i = 0; i < expr.count; ++i) {
        const auto [op, operand] = expr.code[i];
        // Package opcodes 1, 10 and 11 read an acquired flag, a value slot and a literal.
        if (op == 1 || op == 10 || op == 11) {
            if (size == stack.size()) {
                return false;
            }
            std::int32_t value{};
            if (op == 1) {
                const auto at = std::find(kFlagSlots.begin(), kFlagSlots.end(), operand);
                if (at == kFlagSlots.end()) {
                    return false;
                }
                value = state.upgrades[static_cast<std::size_t>(at - kFlagSlots.begin())] == 2;
            } else if (op == 10) {
                if (operand == crafting::kQuestValueSlot) {
                    value = state.questProgress;
                } else if (operand >= crafting::kFirstRuneValueSlot
                           && operand < crafting::kFirstRuneValueSlot + crafting::kRuneCount) {
                    value = state.runes[operand - crafting::kFirstRuneValueSlot];
                } else if (operand >= crafting::kFirstSelectedRuneSlot
                           && operand < crafting::kFirstSelectedRuneSlot + 2) {
                    int rune{};
                    if (!installed_rune(
                            sockets, operand - crafting::kFirstSelectedRuneSlot, rune)) {
                        return false;
                    }
                    value = rune >= 0 ? rune + 1 : 0;
                } else {
                    return false;
                }
            } else {
                if (operand
                    > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
                    return false;
                }
                value = static_cast<std::int32_t>(operand);
            }
            stack[size++] = value;
        } else if (op == 2 || op == 22) {
            if (size == 0) {
                return false;
            }
            // The native evaluator treats opcode 22 as unary integer negation.
            if (op == 22 && stack[size - 1] == (std::numeric_limits<std::int32_t>::min)()) {
                return false;
            }
            stack[size - 1] = op == 2 ? !stack[size - 1] : -stack[size - 1];
        } else {
            if (size < 2) {
                return false;
            }
            const auto right = stack[--size];
            auto& left = stack[size - 1];
            switch (op) {
            case 3:
                left = left != 0 || right != 0;
                break;
            case 4:
                left = left != 0 && right != 0;
                break;
            case 8:
                left = left == right;
                break;
            case 13:
                left = left > right;
                break;
            case 14:
                left = left >= right;
                break;
            default:
                return false;
            }
        }
    }
    if (size != 1) {
        return false;
    }
    result = stack[0] != 0;
    return true;
}

bool allowed(const Plug& plug, const State& state, const inventory::Sockets& sockets) noexcept {
    for (std::size_t i = 0; i < plug.ruleCount; ++i) {
        bool result{};
        if (!evaluate(plug.rules[i], state, sockets, result) || !result) {
            return false;
        }
    }
    return true;
}

bool initialize_sockets(const items::details::Definition& detail,
                        inventory::Sockets& sockets) noexcept {
    if (!inventory::valid(sockets)) {
        return false;
    }
    if (!resolve_socket_choices(detail, sockets)) {
        return false;
    }
    for (std::size_t lane = 0; lane < 8; ++lane) {
        if (!sockets.plugs[lane]) {
            if (lane < 1 || lane > 3) {
                return false;
            }
            continue;
        }
        items::Definition plug{};
        Plug metadata{};
        if (!build_data::find_item_definition_hash(*sockets.plugs[lane], plug)
            || !resolve_plug(plug.definitionIndex, plug, metadata)
            || (plug.definitionIndex != detail.initialPlugIndices[lane]
                && !build_data::is_socket_plug_allowed(
                    kChaliceIndex, static_cast<std::uint8_t>(lane), plug.definitionIndex))) {
            return false;
        }
    }
    return true;
}
} // namespace

bool configure_metadata(std::span<const Plug> plugs) noexcept {
    if (plugs.size() != g_plugs.size()) {
        return false;
    }
    for (std::uint16_t index = kChaliceIndex; index <= crafting::kChaliceLastIndex; ++index) {
        if (!crafting::needs_chalice_item(index)) {
            continue;
        }
        const auto& plug = plugs[index - kChaliceIndex];
        if (!plug.ready || plug.hash == 0
            || (index == kChaliceIndex && plug.hash != kChaliceHash)) {
            return false;
        }
    }
    const std::lock_guard lock{g_metadataMutex};
    std::copy(plugs.begin(), plugs.end(), g_plugs.begin());
    g_metadataReady = true;
    return true;
}

bool metadata_ready() noexcept {
    const std::lock_guard lock{g_metadataMutex};
    return g_metadataReady;
}

bool write(const State& before, const State& after) noexcept {
    if (!valid_state(before) || !valid_state(after)
        || before.questProgress != after.questProgress) {
        return false;
    }
    State current{};
    if (!read_state(current) || current != before) {
        return false;
    }
    for (std::size_t i = 0; i < after.runes.size(); ++i) {
        if (before.runes[i] != after.runes[i]
            && !store::write_unlock(store::Bank::objectiveValues,
                                    static_cast<std::uint16_t>(kRuneValueBase + i),
                                    after.runes[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < after.upgrades.size(); ++i) {
        if (before.upgrades[i] != after.upgrades[i]
            && !store::write_unlock(store::Bank::accountFlags,
                                    static_cast<std::uint16_t>(kUpgradeFlagBase + i),
                                    after.upgrades[i])) {
            return false;
        }
    }
    return true;
}

bool project(const State& before,
             const State& after,
             std::span<std::uint8_t> flags,
             std::span<std::int32_t> values) noexcept {
    if (!valid_state(before) || !valid_state(after) || before.questProgress != after.questProgress
        || flags.size() < kUpgradeFlagBase + after.upgrades.size() || values.size() <= kQuestValue
        || values[kQuestValue] != before.questProgress) {
        return false;
    }
    for (std::size_t i = 0; i < before.runes.size(); ++i) {
        if (values[kRuneValueBase + i] != before.runes[i]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < before.upgrades.size(); ++i) {
        if (flags[kUpgradeFlagBase + i] != before.upgrades[i]) {
            return false;
        }
    }
    std::copy(after.runes.begin(), after.runes.end(), values.begin() + kRuneValueBase);
    std::copy(after.upgrades.begin(), after.upgrades.end(), flags.begin() + kUpgradeFlagBase);
    return true;
}

bool stage(const AccountState& snapshot,
           std::size_t characterIndex,
           std::uint64_t targetInstanceSoid,
           std::uint8_t socketLane,
           std::uint16_t plugDefinitionIndex,
           PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const auto fail = [&](const char* reason) noexcept {
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=chalice_socket result=refused reason=%s lane=%u plug=%u",
                          reason,
                          static_cast<unsigned>(socketLane),
                          static_cast<unsigned>(plugDefinitionIndex));
        return false;
    };
    if (!metadata_ready()) {
        return fail("installed_metadata");
    }
    if (!account::valid(snapshot) || !valid_profile_inventory(snapshot)
        || characterIndex >= snapshot.characterCount || targetInstanceSoid == 0 || socketLane >= 7
        || !snapshot.characters[characterIndex].selected) {
        return fail("request");
    }
    const auto& character = snapshot.characters[characterIndex];
    CharacterItemLocation location{};
    if (!find_character_item_location(character, targetInstanceSoid, location)
        || location.equipped) {
        return fail("held_chalice");
    }
    const auto* target = character_item_at(character, location);
    items::Definition container{}, requested{};
    items::details::Definition detail{};
    buckets::Descriptor bucket{};
    Plug requestedMetadata{};
    if (!target || target->definitionHash != kChaliceHash || target->quantity != 1
        || !build_data::find_item_definition_index(kChaliceIndex, container)
        || container.definitionHash != kChaliceHash
        || !build_data::find_configured_item_detail(kChaliceIndex, detail)
        || detail.definitionIndex != kChaliceIndex || detail.definitionHash != kChaliceHash
        || detail.bucketId != container.bucketId || detail.equipmentSlot.has_value()
        || detail.instancedDefinitionState != items::details::InstancedDefinitionState::instanced
        || detail.maxStackSize != 1 || detail.ordinarySocketCount != 8
        || detail.ordinarySocketState != items::details::OrdinarySocketState::present
        || !build_data::find_inventory_bucket_descriptor(container.bucketId, bucket)
        || bucket.arraySelector != buckets::ArraySelector::character
        || !resolve_plug(plugDefinitionIndex, requested, requestedMetadata)
        || !build_data::is_socket_plug_allowed(kChaliceIndex, socketLane, plugDefinitionIndex)) {
        return fail("definition");
    }
    for (std::size_t lane = 0; lane < 8; ++lane) {
        if (detail.socketTypes[lane] != kSocketTypes[lane]) {
            return fail("socket_type");
        }
    }
    auto sockets = target->sockets;
    State before{};
    if (!initialize_sockets(detail, sockets) || !read_state(before)) {
        return fail("saved_state");
    }
    auto after = before;
    AccountState candidate = snapshot;
    materials::Definition costs{};
    if (!allowed(requestedMetadata, before, sockets)) {
        return fail("native_insertion_predicate");
    }

    if (socketLane >= 4) {
        const auto lane = static_cast<std::size_t>(socketLane - 4);
        const int next = rune_index(plugDefinitionIndex, lane);
        const bool removing = plugDefinitionIndex == 7938 + lane;
        int previous{};
        if ((!removing && next < 0) || before.upgrades[lane] != 2
            || !installed_rune(sockets, lane, previous)
            || requested.insertionMaterialRequirementSetIndex != materials::kUnavailableSetIndex) {
            return fail("rune_slot_or_cost");
        }
        if (next == previous) {
            return fail("already_applied");
        }
        if (next >= 0) {
            const auto color = static_cast<std::size_t>(next) / 3;
            if (color != 0 && before.upgrades[2 + color] != 2) {
                return fail("rune_compatibility");
            }
            if (before.runes[next] == 0) {
                return fail("rune_balance");
            }
            --after.runes[next];
        } else {
            // Native cascading removal is not decoded. Require removal from III toward I.
            for (std::size_t later = lane + 1; later < 3; ++later) {
                int held{};
                if (!installed_rune(sockets, later, held) || held >= 0) {
                    return fail("remove_later_runes_first");
                }
            }
        }
        // Sunrise reservation policy: slotting removes one available rune; replacing or
        // removing it returns that reservation. No Menagerie chest payout/consumption runs.
        if (previous >= 0) {
            if (after.runes[previous] == (std::numeric_limits<std::int32_t>::max)()) {
                return fail("refund_overflow");
            }
            ++after.runes[previous];
        }
        sockets.plugs[socketLane] = requested.definitionHash;
    } else {
        std::size_t flag = 0;
        std::uint16_t resultIndex = 8019;
        if (socketLane == 0) {
            if (plugDefinitionIndex < 8015 || plugDefinitionIndex > 8017) {
                return fail("unsupported_upgrade_action");
            }
            flag = plugDefinitionIndex - 8015;
        } else {
            const auto first = 7998 + (socketLane - 1) * 6;
            if (plugDefinitionIndex < first || plugDefinitionIndex > first + 4
                || (plugDefinitionIndex - first) % 2 != 0) {
                return fail("purchase_required");
            }
            const auto tier = (plugDefinitionIndex - first) / 2;
            flag = 3 + (socketLane - 1) * 3 + tier;
            if (tier != 0 && before.upgrades[flag - 1] != 2) {
                return fail("prior_upgrade");
            }
            resultIndex = static_cast<std::uint16_t>(plugDefinitionIndex - 1);
        }
        if (before.upgrades[flag] == 2) {
            return fail("upgrade_owned");
        }
        items::Definition imperials{}, result{};
        items::details::Definition imperialDetail{};
        Plug resultMetadata{};
        buckets::Descriptor imperialBucket{};
        const auto set = requested.insertionMaterialRequirementSetIndex;
        if (set == materials::kUnavailableSetIndex
            || !build_data::find_material_requirement_set(set, costs)
            || costs.requirementSetIndex != set || costs.requirementCount != 1
            || !build_data::find_item_definition_hash(kImperialsHash, imperials)
            || imperials.definitionHash != kImperialsHash
            || !build_data::find_configured_item_detail(imperials.definitionIndex, imperialDetail)
            || imperialDetail.definitionHash != kImperialsHash
            || imperialDetail.definitionIndex != imperials.definitionIndex
            || imperialDetail.bucketId != imperials.bucketId || imperialDetail.maxStackSize <= 0
            || imperialDetail.instancedDefinitionState
                   != items::details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(imperials.bucketId, imperialBucket)
            || imperialBucket.arraySelector != buckets::ArraySelector::profile) {
            return fail("imperial_cost_definition");
        }
        const auto& cost = costs.requirements[0];
        if (cost.itemDefinitionIndex != imperials.definitionIndex || cost.quantity == 0
            || !cost.deleteOnAction || cost.omitFromRequirements
            || cost.condition != materials::kUnconditionalRequirement) {
            return fail("imperial_cost_shape");
        }
        after.upgrades[flag] = 2;
        if (!resolve_plug(resultIndex, result, resultMetadata)
            || result.insertionMaterialRequirementSetIndex != materials::kUnavailableSetIndex
            || !allowed(resultMetadata, after, sockets)) {
            return fail("upgrade_result");
        }
        bool charged = false;
        if (!apply_action_materials(snapshot, costs, candidate, charged) || !charged) {
            return fail("imperial_balance");
        }
        sockets.plugs[socketLane] = result.definitionHash;
        // Perfected and activity/drop modifiers are deliberately not synthesized.
    }

    auto* changed = character_item_at(candidate.characters[characterIndex], location);
    if (!changed) {
        return fail("target_copy");
    }
    changed->sockets = sockets;
    if (!valid_state(after)) {
        return fail("after_image");
    }
    mutation.beforeChalice = before;
    mutation.afterChalice = after;
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
    return true;
}
} // namespace sunrise::state::runtime::detail::chalice
