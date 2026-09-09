#include <shared_mutex>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/kill_incident.h"
#include "../../state/build_data/combat_labels/catalog.h"
#include "../../state/build_data/enemy_classes/enemy_class_catalog.h"
#include "../../state/build_data/sobjects/sobject_catalog.h"
#include "../../state/runtime/gameplay_investment_runtime.h"
#include "encrypted/push/activity/internal.h"
#include "gameplay_bubble_context.h"
#include "gameplay_investment.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {
GameplayInvestmentStatus g_status{};
constexpr std::uint32_t name_hash(std::string_view name) noexcept {
    std::uint32_t hash = 0x811C9DC5U;
    for (const unsigned char c : name)
        hash = (hash * 0x01000193U) ^ c;
    return hash;
}
void report(const char* reason) noexcept {
    ++g_status.refused;
    g_status.lastReason = reason;
    core::log::writef(core::log::Channel::server,
                      core::log::Level::debug,
                      "ev=bounty_gameplay result=refused reason=%s",
                      reason);
}
} // namespace
GameplayInvestmentStatus gameplay_investment_status() noexcept {
    const std::shared_lock lock(session_lock());
    auto result = g_status;
    result.enemyClasses = state::build_data::enemy_classes::count();
    result.combatLabels = state::build_data::combat_labels::count();
    return result;
}
void invest_gameplay_incident_locked(
    const ActivityClientBinding& binding,
    const middleware::bap::activity_message::incident::Incident& incident) noexcept {
    namespace wire = middleware::bap::activity_message::kill_incident;
    namespace labels = state::build_data::combat_labels;
    namespace classes = state::build_data::enemy_classes;
    namespace gameplay = state::bounties::gameplay;
    // The installed sobject table selects semantics. A numeric target alone never grants credit.
    state::build_data::sobjects::Definition definition{};
    if (incident.primaryTarget > 0xFFFFU
        || !state::build_data::sobjects::find(static_cast<std::uint16_t>(incident.primaryTarget),
                                              definition)
        || definition.nameHash != name_hash("any_kill") || definition.typeCode != 1)
        return;
    wire::Payload decoded{};
    if (incident.payloadLength > incident.payload.size()
        || !wire::decode_raw(std::span(incident.payload).first(incident.payloadLength), decoded)) {
        report("unsupported_payload");
        return;
    }
    ++g_status.decoded;
    std::size_t links{};
    const auto* session = unique_activity_link_locked(binding.session, links);
    if (!session || links != 1 || !session->authenticated
        || session->activity.bindingGeneration != binding.bindingGeneration
        || (binding.role != ActivityClientRole::publicTarget
            && session->activityJoinGeneration != binding.bindingGeneration)) {
        report("stale_link");
        return;
    }
    const auto playerKey = encrypted::push::activity::published_player_key(*session);
    if (!playerKey || !session->activityCharacterSoid || decoded.recipient != playerKey
        || decoded.killer.player != playerKey || decoded.victim.player.value_or(0) != 0
        || !decoded.victim.combatant || !decoded.victim.classHash || !decoded.reorderSequence) {
        report("attribution");
        return;
    }
    std::array<std::byte, 40> sourceMask{}, actorMask{}, victimMask{};
    if (!labels::mask_for_labels(decoded.source.view(), sourceMask)
        || !labels::mask_for_labels(decoded.actor.view(), actorMask)
        || !labels::mask_for_labels(decoded.target.view(), victimMask)) {
        report("label_catalog");
        return;
    }
    std::uint32_t race{};
    const auto lookup = classes::classify(-1, *decoded.victim.classHash, race);
    const auto modifier = labels::classify_victim(victimMask);
    if (modifier == labels::VictimRace::invalid || lookup == classes::Lookup::invalid
        || lookup == classes::Lookup::nonEnemy || lookup == classes::Lookup::unavailable) {
        report("victim_class");
        return;
    }
    if (modifier == labels::VictimRace::scorn) {
        if (race && race != name_hash("scorn")) {
            report("victim_conflict");
            return;
        }
        race = name_hash("scorn");
    }
    if (!race) {
        report("unknown_enemy");
        return;
    }
    const auto source = labels::classify(sourceMask, actorMask);
    gameplay::Event event{};
    const auto& owner = binding.source;
    std::size_t ownerLinks{};
    const auto* ownerSession = unique_activity_link_locked(owner, ownerLinks);
    if (!ownerSession || ownerLinks != 1
        || ownerSession->activity.role == ActivityClientRole::publicTarget
        || ownerSession->activityJoinGeneration != ownerSession->activity.bindingGeneration
        || ownerSession->activityCharacterSoid != session->activityCharacterSoid) {
        report("owner_link");
        return;
    }
    event.context = {owner.sessionId, owner.createdRevision, binding.bindingGeneration};
    event.sequence = decoded.reorderSequence;
    event.kind = gameplay::Kind::authoredKill;
    event.creditedCharacter = session->activityCharacterSoid;
    event.activityIndex = static_cast<std::uint16_t>(owner.destination.activityIndex);
    event.victimClassHash = decoded.victim.classHash;
    event.raceHash = race;
    event.victimRankHash = labels::classify_victim_rank(victimMask);
    event.bubble = gameplay_bubble_locked(*ownerSession);
    // Same killing-source predicate as the working branch, using its transported selectors.
    // A non-sentinel ability selector bypasses equipment lookup; never substitute the current gun.
    if (source.attributed && source.weapon && source.weaponClass != 0 && !source.melee
        && !source.grenade && !source.superAbility && !source.ability
        && source.abilityLabelHash == 0 && decoded.abilitySelector == -1
        && decoded.equipmentSlot) {
        const auto slot = static_cast<gameplay::WeaponSlot>(*decoded.equipmentSlot);
        if (gameplay::valid_weapon_slot(slot)) event.weaponSlot = slot;
    }
    if (source.attributed) {
        event.weaponClass = source.weaponClass;
        event.weaponKill = source.weapon;
        event.grenadeKill = source.grenade;
        event.superKill = source.superAbility;
        event.meleeKill = source.melee;
        event.abilityKill = source.ability;
        event.precision = source.precision;
        event.abilityLabelHash = source.abilityLabelHash;
        event.playerClassHash = source.playerClassHash;
        if (decoded.damage >= 0 && decoded.damage <= 3)
            event.damage = static_cast<gameplay::Damage>(decoded.damage);
    }
    const auto result =
        state::invest_gameplay_kill(event, owner, playerKey, decoded.reorderSequence);
    if (result.status == state::GameplayKillStatus::duplicate) {
        ++g_status.duplicates;
        g_status.lastReason = "duplicate";
    } else if (result.status == state::GameplayKillStatus::applied
               || result.status == state::GameplayKillStatus::acceptedNoChange) {
        ++g_status.accepted;
        g_status.lanes += result.applied;
        g_status.triumphs += result.triumphsAdvanced;
        g_status.lastReason = result.applied ? "credited" : "no_matching_held_lane";
    } else {
        switch (result.status) {
        case state::GameplayKillStatus::invalidContext: report("invalid_context"); break;
        case state::GameplayKillStatus::wrongCharacter: report("wrong_character"); break;
        case state::GameplayKillStatus::invalidAccount: report("invalid_account"); break;
        case state::GameplayKillStatus::capacity: report("capacity"); break;
        default: report("invalid_event"); break;
        }
    }
    if (result.changedCount || result.triumphsAdvanced) arm_account_resync_everywhere();
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=bounty_gameplay result=%u lanes=%zu target=%u race=%08X weapon=%08X "
                      "damage=%d precision=%u source=%u activity=%u status=%s",
                      static_cast<unsigned>(result.status),
                      result.applied,
                      incident.primaryTarget,
                      race,
                      source.weaponClass,
                      decoded.damage,
                      unsigned(source.precision),
                      unsigned(source.attributed),
                      owner.destination.activityIndex,
                      g_status.lastReason);
}
} // namespace sunrise::server::bap
