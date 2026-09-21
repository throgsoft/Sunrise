#include "chalice_crafting_runtime.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

#include "../../core/logging/log.h"
#include "../../middleware/content/packages/tables/definition_index_table.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail::chalice {
namespace {
namespace items = build_data::items;
namespace materials = build_data::material_requirements;
namespace inventory = account::inventory;
namespace buckets = build_data::inventory::buckets;
namespace tables = middleware::content::packages::tables;
namespace store = investment::store;

constexpr std::uint16_t kChaliceIndex = 7937;
constexpr std::uint16_t kQuestValue = 5662;
constexpr std::uint32_t kImperialsHash = 1642918584U;
constexpr std::array<std::uint16_t, 13> kFlagSlots{
    8698, 8699, 8700, 8703, 8704, 8705, 8706, 8707, 8708, 8709, 8710, 8711, 8712};
constexpr std::array<std::uint16_t, 8> kSocketTypes{615, 611, 612, 613, 619, 620, 621, 614};
constexpr std::array<std::uint32_t, 8> kSocketHashes{
    2609924932U, 1250700346U, 3195245497U, 3276661397U,
    3422549977U, 3422549976U, 3422549979U, 1639095227U};

struct Instruction {
    std::uint32_t op{}, operand{};
};
struct Expression {
    std::array<Instruction, 8> code{};
    std::size_t count{};
};
struct Plug {
    std::uint32_t hash{};
    std::array<Expression, 3> rules{};
    std::size_t ruleCount{};
    bool ready{};
};
std::mutex g_metadataMutex;
std::array<Plug, 83> g_plugs{};
bool g_banksReady{}, g_socketsReady{};

template <typename T>
bool read(std::span<const std::byte> bytes, std::size_t at, T& value) noexcept {
    if (at > bytes.size() || sizeof(T) > bytes.size() - at) return false;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return true;
}

bool array(std::span<const std::byte> bytes, std::size_t at, std::uint32_t cls,
           std::size_t stride, tables::Array& result) noexcept {
    return tables::find_array_at(bytes, at, result) && result.elementClass == cls
           && result.dataOffset <= bytes.size()
           && result.count <= (bytes.size() - result.dataOffset) / stride;
}

bool expression(std::span<const std::byte> bytes, std::size_t at, Expression& result) noexcept {
    tables::Array code{};
    if (!array(bytes, at, 0x80807D31U, 8, code) || code.count == 0
        || code.count > result.code.size()) return false;
    result.count = static_cast<std::size_t>(code.count);
    for (std::size_t i = 0; i < result.count; ++i)
        if (!read(bytes, code.dataOffset + i * 8, result.code[i])) return false;
    return true;
}

bool valid_state(const State& state) noexcept {
    return state.questProgress >= -1
           && std::all_of(state.runes.begin(), state.runes.end(), [](auto v) { return v >= 0; })
           && std::all_of(state.upgrades.begin(), state.upgrades.end(), [](auto v) { return v <= 2; });
}

bool read_state(State& state) noexcept {
    const std::lock_guard lock{store::g_mutex};
    state = {};
    for (std::size_t i = 0; i < state.runes.size(); ++i)
        if (!store::read_unlock(store::Bank::objectiveValues,
                                static_cast<std::uint16_t>(kRuneValueBase + i), state.runes[i]))
            return false;
    for (std::size_t i = 0; i < state.upgrades.size(); ++i) {
        std::int32_t flag{};
        if (!store::read_unlock(store::Bank::accountFlags,
                                static_cast<std::uint16_t>(kUpgradeFlagBase + i), flag)
            || flag < 0 || flag > 2) return false;
        state.upgrades[i] = static_cast<std::uint8_t>(flag);
    }
    return store::read_unlock(store::Bank::objectiveValues, kQuestValue, state.questProgress)
           && valid_state(state);
}

bool resolve_plug(std::uint16_t index, items::Definition& definition, Plug& metadata) noexcept {
    if (!needs_item(index) || index == kChaliceIndex) return false;
    {
        const std::lock_guard lock{g_metadataMutex};
        metadata = g_plugs[index - kChaliceIndex];
    }
    return metadata.ready && build_data::find_item_definition_index(index, definition)
           && definition.definitionIndex == index && definition.definitionHash == metadata.hash;
}

int rune_index(std::uint16_t plug, std::size_t lane) noexcept {
    const auto first = 7949 + lane * 12;
    return plug >= first && plug < first + 12 ? static_cast<int>(plug - first) : -1;
}

bool installed_rune(const inventory::Sockets& sockets, std::size_t lane, int& rune) noexcept {
    rune = -1;
    if (lane >= 3 || !sockets.plugs[4 + lane]) return false;
    items::Definition plug{};
    Plug metadata{};
    if (!build_data::find_item_definition_hash(*sockets.plugs[4 + lane], plug)
        || !resolve_plug(plug.definitionIndex, plug, metadata)) return false;
    rune = rune_index(plug.definitionIndex, lane);
    return rune >= 0 || plug.definitionIndex == 7938 + lane;
}

/** Only the decoded Chalice expression subset is admitted. Unknown sources fail closed. */
bool evaluate(const Expression& expr, const State& state, const inventory::Sockets& sockets,
              bool& result) noexcept {
    std::array<std::int32_t, 8> stack{};
    std::size_t size = 0;
    for (std::size_t i = 0; i < expr.count; ++i) {
        const auto [op, operand] = expr.code[i];
        if (op == 1 || op == 10 || op == 11) {
            if (size == stack.size()) return false;
            std::int32_t value{};
            if (op == 1) {
                const auto at = std::find(kFlagSlots.begin(), kFlagSlots.end(), operand);
                if (at == kFlagSlots.end()) return false;
                value = state.upgrades[static_cast<std::size_t>(at - kFlagSlots.begin())] == 2;
            } else if (op == 10) {
                if (operand == 12823) value = state.questProgress;
                else if (operand >= 5512 && operand < 5524) value = state.runes[operand - 5512];
                else if (operand == 5503 || operand == 5504) {
                    int rune{};
                    if (!installed_rune(sockets, operand - 5503, rune)) return false;
                    value = rune >= 0 ? rune + 1 : 0;
                } else return false;
            } else {
                if (operand > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
                    return false;
                value = static_cast<std::int32_t>(operand);
            }
            stack[size++] = value;
        } else if (op == 2 || op == 22) {
            if (size == 0) return false;
            // Native 554310 implements opcode 22 as unary integer negation.
            if (op == 22 && stack[size - 1] == (std::numeric_limits<std::int32_t>::min)())
                return false;
            stack[size - 1] = op == 2 ? !stack[size - 1] : -stack[size - 1];
        } else {
            if (size < 2) return false;
            const auto right = stack[--size];
            auto& left = stack[size - 1];
            switch (op) {
            case 3: left = left != 0 || right != 0; break;
            case 4: left = left != 0 && right != 0; break;
            case 8: left = left == right; break;
            case 13: left = left > right; break;
            case 14: left = left >= right; break;
            default: return false;
            }
        }
    }
    if (size != 1) return false;
    result = stack[0] != 0;
    return true;
}

bool allowed(const Plug& plug, const State& state, const inventory::Sockets& sockets) noexcept {
    for (std::size_t i = 0; i < plug.ruleCount; ++i) {
        bool result{};
        if (!evaluate(plug.rules[i], state, sockets, result) || !result) return false;
    }
    return true;
}

bool initialize_sockets(const items::details::Definition& detail,
                         inventory::Sockets& sockets) noexcept {
    if (!inventory::valid(sockets)) return false;
    if (sockets.policy == inventory::SocketPolicy::nativeDefaults) {
        sockets = {};
        sockets.policy = inventory::SocketPolicy::authored;
        sockets.plugCount = 8;
        for (std::size_t lane = 0; lane < 8; ++lane) {
            const auto index = detail.initialPlugIndices[lane];
            if (index == items::details::kUnavailableItemIndex) continue;
            items::Definition plug{};
            Plug metadata{};
            if (!resolve_plug(index, plug, metadata)) return false;
            sockets.plugs[lane] = plug.definitionHash;
        }
    }
    if (sockets.policy != inventory::SocketPolicy::authored || sockets.plugCount != 8) return false;
    for (std::size_t lane = 0; lane < 8; ++lane) {
        if (!sockets.plugs[lane]) {
            if (lane < 1 || lane > 3) return false;
            continue;
        }
        items::Definition plug{};
        if (!build_data::find_item_definition_hash(*sockets.plugs[lane], plug)
            || (plug.definitionIndex != detail.initialPlugIndices[lane]
                && !build_data::is_socket_plug_allowed(kChaliceIndex,
                    static_cast<std::uint8_t>(lane), plug.definitionIndex))) return false;
    }
    return true;
}
} // namespace

bool configure_banks(std::span<const std::byte> flagSlots, std::span<const std::byte> valueSlots,
                     std::span<const std::byte> flagMaps, std::span<const std::byte> valueMaps) noexcept {
    const std::lock_guard lock{g_metadataMutex};
    g_banksReady = false;
    tables::Array flags{}, values{}, flagMap{}, valueMap{};
    if (!array(flagSlots, 8, 0x80807D4FU, 8, flags)
        || !array(valueSlots, 8, 0x80807C96U, 8, values)
        || !array(flagMaps, 8, 0x80807D48U, 8, flagMap)
        || !array(valueMaps, 8, 0x80807C8DU, 8, valueMap)) return false;
    const auto mapping = [](auto sourceBytes, const auto& sources, auto mapBytes,
                             const auto& maps, std::uint16_t slot, std::uint16_t bank,
                             std::uint8_t aggregation) noexcept {
        std::uint8_t kind{}, flags{};
        std::uint16_t mapped{}, reverse{};
        std::uint32_t sourceHash{}, mapHash{};
        const auto at = sources.dataOffset + static_cast<std::size_t>(slot) * 8;
        const auto dest = maps.dataOffset + static_cast<std::size_t>(bank) * 8;
        return slot < sources.count && bank < maps.count
               && read(sourceBytes, at, sourceHash) && read(mapBytes, dest, mapHash)
               && sourceHash == mapHash && read(sourceBytes, at + 4, kind) && kind == 1
               && read(sourceBytes, at + 5, flags) && flags == aggregation
               && read(sourceBytes, at + 6, mapped) && mapped == bank
               && read(mapBytes, dest + 4, reverse) && reverse == slot;
    };
    for (std::size_t i = 0; i < kFlagSlots.size(); ++i)
        if (!mapping(flagSlots, flags, flagMaps, flagMap, kFlagSlots[i],
                     static_cast<std::uint16_t>(kUpgradeFlagBase + i), 0)) return false;
    for (std::size_t i = 0; i < 12; ++i)
        if (!mapping(valueSlots, values, valueMaps, valueMap,
                     static_cast<std::uint16_t>(5512 + i),
                     static_cast<std::uint16_t>(kRuneValueBase + i), 0)) return false;
    for (std::uint16_t slot = 5503; slot <= 5504; ++slot) {
        std::uint8_t kind{}, aggregation{};
        std::uint16_t bank{};
        const auto at = values.dataOffset + static_cast<std::size_t>(slot) * 8;
        if (slot >= values.count || !read(valueSlots, at + 4, kind) || kind != 4
            || !read(valueSlots, at + 5, aggregation) || aggregation != 0
            || !read(valueSlots, at + 6, bank) || bank != 0xFFFFU) return false;
    }
    if (!mapping(valueSlots, values, valueMaps, valueMap, 12823, kQuestValue, 2)) return false;
    g_banksReady = true;
    return true;
}

bool configure_sockets(std::span<const std::byte> bytes) noexcept {
    const std::lock_guard lock{g_metadataMutex};
    g_socketsReady = false;
    tables::Array types{};
    if (!array(bytes, 8, tables::kSocketTypeTableClass, tables::kSocketTypeRowStride, types))
        return false;
    for (std::size_t lane = 0; lane < kSocketTypes.size(); ++lane) {
        const auto at = types.dataOffset + kSocketTypes[lane] * tables::kSocketTypeRowStride;
        std::uint32_t hash{};
        tables::Array scalars{};
        if (kSocketTypes[lane] >= types.count || !read(bytes, at, hash) || hash != kSocketHashes[lane]
            || !tables::find_optional_array_at(bytes, at + 56, scalars) || scalars.count != 0)
            return false;
    }
    g_socketsReady = true;
    return true;
}

bool needs_item(std::uint16_t index) noexcept {
    return (index >= 7937 && index <= 7940) || (index >= 7949 && index <= 7984)
           || (index >= 7997 && index <= 8019);
}

bool configure_item(std::uint16_t index, std::uint32_t hash,
                     std::span<const std::byte> bytes) noexcept {
    if (!needs_item(index) || hash == 0) return false;
    const std::lock_guard lock{g_metadataMutex};
    auto& output = g_plugs[index - kChaliceIndex];
    output = {};
    if (index == kChaliceIndex) {
        if (hash != kChaliceHash) return false;
    } else {
        std::int64_t relative{};
        std::uint32_t cls{};
        if (!read(bytes, 64, relative) || relative <= 0
            || bytes.size() < 64 || static_cast<std::uint64_t>(relative) > bytes.size() - 64)
            return false;
        const auto at = 64 + static_cast<std::size_t>(relative);
        if (!read(bytes, at - 4, cls) || cls != 0x808077E3U) return false;
        tables::Array rules{};
        if (!tables::find_optional_array_at(bytes, at + 64, rules)
            || rules.count > output.rules.size()
            || (rules.count != 0 && (rules.elementClass != 0x808077E6U
                || rules.dataOffset > bytes.size()
                || rules.count > (bytes.size() - rules.dataOffset) / 16))) return false;
        output.ruleCount = static_cast<std::size_t>(rules.count);
        for (std::size_t i = 0; i < output.ruleCount; ++i)
            if (!expression(bytes, rules.dataOffset + i * 16, output.rules[i])) return false;
        if (index >= 7949 && index <= 7984) {
            Expression quantity{};
            if (!expression(bytes, at + 224, quantity) || quantity.count != 1
                || quantity.code[0].op != 10
                || quantity.code[0].operand != 5512U + (index - 7949U) % 12U) return false;
        }
    }
    output.hash = hash;
    output.ready = true;
    return true;
}

bool metadata_ready() noexcept {
    const std::lock_guard lock{g_metadataMutex};
    if (!g_banksReady || !g_socketsReady) return false;
    for (std::uint16_t index = 7937; index <= 8019; ++index)
        if (needs_item(index) && !g_plugs[index - kChaliceIndex].ready) return false;
    return true;
}

bool write(const State& before, const State& after) noexcept {
    if (!valid_state(before) || !valid_state(after) || before.questProgress != after.questProgress)
        return false;
    State current{};
    if (!read_state(current) || current != before) return false;
    for (std::size_t i = 0; i < after.runes.size(); ++i)
        if (before.runes[i] != after.runes[i]
            && !store::write_unlock(store::Bank::objectiveValues,
                 static_cast<std::uint16_t>(kRuneValueBase + i), after.runes[i])) return false;
    for (std::size_t i = 0; i < after.upgrades.size(); ++i)
        if (before.upgrades[i] != after.upgrades[i]
            && !store::write_unlock(store::Bank::accountFlags,
                 static_cast<std::uint16_t>(kUpgradeFlagBase + i), after.upgrades[i])) return false;
    return true;
}

bool project(const State& before, const State& after, std::span<std::uint8_t> flags,
              std::span<std::int32_t> values) noexcept {
    if (!valid_state(before) || !valid_state(after) || before.questProgress != after.questProgress
        || flags.size() < kUpgradeFlagBase + after.upgrades.size() || values.size() <= kQuestValue
        || values[kQuestValue] != before.questProgress) return false;
    for (std::size_t i = 0; i < before.runes.size(); ++i)
        if (values[kRuneValueBase + i] != before.runes[i]) return false;
    for (std::size_t i = 0; i < before.upgrades.size(); ++i)
        if (flags[kUpgradeFlagBase + i] != before.upgrades[i]) return false;
    std::copy(after.runes.begin(), after.runes.end(), values.begin() + kRuneValueBase);
    std::copy(after.upgrades.begin(), after.upgrades.end(), flags.begin() + kUpgradeFlagBase);
    return true;
}

bool stage(const AccountState& snapshot, std::size_t characterIndex,
            std::uint64_t targetInstanceSoid, std::uint8_t socketLane,
            std::uint16_t plugDefinitionIndex, PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const auto fail = [&](const char* reason) noexcept {
        core::log::writef(core::log::Channel::state, core::log::Level::warn,
                          "ev=chalice_socket result=refused reason=%s lane=%u plug=%u",
                          reason, static_cast<unsigned>(socketLane),
                          static_cast<unsigned>(plugDefinitionIndex));
        return false;
    };
    if (!metadata_ready()) return fail("installed_metadata");
    if (!account::valid(snapshot) || !valid_profile_inventory(snapshot)
        || characterIndex >= snapshot.characterCount || targetInstanceSoid == 0
        || socketLane >= 7 || !snapshot.characters[characterIndex].selected)
        return fail("request");
    const auto& character = snapshot.characters[characterIndex];
    CharacterItemLocation location{};
    if (!find_character_item_location(character, targetInstanceSoid, location) || location.equipped)
        return fail("held_chalice");
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
        || !build_data::is_socket_plug_allowed(kChaliceIndex, socketLane, plugDefinitionIndex))
        return fail("definition");
    for (std::size_t lane = 0; lane < 8; ++lane)
        if (detail.socketTypes[lane] != kSocketTypes[lane]) return fail("socket_type");
    auto sockets = target->sockets;
    State before{};
    if (!initialize_sockets(detail, sockets) || !read_state(before)) return fail("saved_state");
    auto after = before;
    AccountState candidate = snapshot;
    materials::Definition costs{};
    if (!allowed(requestedMetadata, before, sockets)) return fail("native_insertion_predicate");

    if (socketLane >= 4) {
        const auto lane = static_cast<std::size_t>(socketLane - 4);
        const int next = rune_index(plugDefinitionIndex, lane);
        const bool removing = plugDefinitionIndex == 7938 + lane;
        int previous{};
        if ((!removing && next < 0) || before.upgrades[lane] != 2
            || !installed_rune(sockets, lane, previous)
            || requested.insertionMaterialRequirementSetIndex != materials::kUnavailableSetIndex)
            return fail("rune_slot_or_cost");
        if (next == previous) return fail("already_applied");
        if (next >= 0) {
            const auto color = static_cast<std::size_t>(next) / 3;
            if (color != 0 && before.upgrades[2 + color] != 2) return fail("rune_compatibility");
            if (before.runes[next] == 0) return fail("rune_balance");
            --after.runes[next];
        } else {
            // Native cascading removal is not decoded. Require removal from III toward I.
            for (std::size_t later = lane + 1; later < 3; ++later) {
                int held{};
                if (!installed_rune(sockets, later, held) || held >= 0)
                    return fail("remove_later_runes_first");
            }
        }
        // Sunrise reservation policy: slotting removes one available rune; replacing or
        // removing it returns that reservation. No Menagerie chest payout/consumption runs.
        if (previous >= 0) {
            if (after.runes[previous] == (std::numeric_limits<std::int32_t>::max)())
                return fail("refund_overflow");
            ++after.runes[previous];
        }
        sockets.plugs[socketLane] = requested.definitionHash;
    } else {
        std::size_t flag = 0;
        std::uint16_t resultIndex = 8019;
        if (socketLane == 0) {
            if (plugDefinitionIndex < 8015 || plugDefinitionIndex > 8017)
                return fail("unsupported_upgrade_action");
            flag = plugDefinitionIndex - 8015;
        } else {
            const auto first = 7998 + (socketLane - 1) * 6;
            if (plugDefinitionIndex < first || plugDefinitionIndex > first + 4
                || (plugDefinitionIndex - first) % 2 != 0)
                return fail("purchase_required");
            const auto tier = (plugDefinitionIndex - first) / 2;
            flag = 3 + (socketLane - 1) * 3 + tier;
            if (tier != 0 && before.upgrades[flag - 1] != 2) return fail("prior_upgrade");
            resultIndex = static_cast<std::uint16_t>(plugDefinitionIndex - 1);
        }
        if (before.upgrades[flag] == 2) return fail("upgrade_owned");
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
            || imperialDetail.instancedDefinitionState != items::details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(imperials.bucketId, imperialBucket)
            || imperialBucket.arraySelector != buckets::ArraySelector::profile)
            return fail("imperial_cost_definition");
        const auto& cost = costs.requirements[0];
        if (cost.itemDefinitionIndex != imperials.definitionIndex || cost.quantity == 0
            || !cost.deleteOnAction || cost.omitFromRequirements
            || cost.condition != materials::kUnconditionalRequirement)
            return fail("imperial_cost_shape");
        after.upgrades[flag] = 2;
        if (!resolve_plug(resultIndex, result, resultMetadata)
            || result.insertionMaterialRequirementSetIndex != materials::kUnavailableSetIndex
            || !allowed(resultMetadata, after, sockets)) return fail("upgrade_result");
        bool charged = false;
        if (!apply_action_materials(snapshot, costs, candidate, charged) || !charged)
            return fail("imperial_balance");
        sockets.plugs[socketLane] = result.definitionHash;
        // Perfected and activity/drop modifiers are deliberately not synthesized.
    }

    auto* changed = character_item_at(candidate.characters[characterIndex], location);
    if (!changed) return fail("target_copy");
    changed->sockets = sockets;
    middleware::datagen::family4::loadout::ResolvedLoadout beforeLoadout{}, afterLoadout{};
    ResolvedPosition beforePosition{}, afterPosition{};
    if (!valid_state(after) || !account::valid(candidate) || !valid_profile_inventory(candidate)
        || !middleware::datagen::family4::loadout::resolve(snapshot, characterIndex, beforeLoadout)
        || !middleware::datagen::family4::loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) return fail("after_image");
    items::Definition result{};
    if (!build_data::find_item_definition_hash(sockets.plugs[socketLane].value_or(0), result))
        return fail("result");
    mutation.beforeCharacter = character;
    mutation.afterCharacter = candidate.characters[characterIndex];
    mutation.beforeProfileItems = snapshot.profileItems;
    mutation.afterProfileItems = candidate.profileItems;
    mutation.beforeChalice = before;
    mutation.afterChalice = after;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = character.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.targetDefinitionHash = kChaliceHash;
    mutation.plugDefinitionHash = result.definitionHash;
    mutation.materialRequirementSetHash = costs.requirementSetHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedProfileItemCount = snapshot.profileItemCount;
    mutation.afterProfileItemCount = candidate.profileItemCount;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = kChaliceIndex;
    mutation.plugDefinitionIndex = result.definitionIndex;
    mutation.requestedPlugDefinitionIndex = plugDefinitionIndex;
    mutation.materialRequirementSetIndex = costs.requirementSetIndex;
    mutation.socketLane = socketLane;
    mutation.targetBucketId = container.bucketId;
    mutation.plugBucketId = result.bucketId;
    mutation.materialRequirementCount = costs.requirementCount;
    // This flag requests the account object for native counter/flag changes as well as stacks.
    mutation.profileChanged = true;
    mutation.targetEquipped = false;
    mutation.prepared = true;
    return true;
}
} // namespace sunrise::state::runtime::detail::chalice
