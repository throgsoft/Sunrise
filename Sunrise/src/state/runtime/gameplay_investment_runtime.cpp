#include "gameplay_investment_runtime.h"

#include "../../core/runtime/wall_clock.h"
#include "../activity/runtime.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/unlocks_records.h"

namespace sunrise::state {
namespace {
bool mapping_valid(bounties::KillRuleDefinition& rule) noexcept {
    if (rule.lane < account::inventory::kItemObjectiveLaneBase
        || rule.lane >= account::inventory::kItemObjectiveCapacity)
        return false;
    const auto ordinal = rule.lane - account::inventory::kItemObjectiveLaneBase;
    build_data::items::Definition item{};
    build_data::items::details::Definition detail{};
    build_data::objectives::Definition objective{};
    if (!(build_data::find_item_definition_hash(rule.itemHash, item)
          && build_data::find_configured_item_detail(item.definitionIndex, detail)
          && detail.definitionHash == rule.itemHash
          && (!rule.nonExpiringQuest || detail.lifetimeSeconds == 0)
          && ordinal < detail.objectiveCount && ordinal < detail.objectiveIndices.size()
          && build_data::find_objective_definition(detail.objectiveIndices[ordinal], objective)
          && objective.completionValue > 0))
        return false;
    rule.itemIndex = item.definitionIndex;
    rule.objectiveIndex = detail.objectiveIndices[ordinal];
    rule.objectiveHash = objective.definitionHash;
    rule.completion = objective.completionValue;
    return true;
}
} // namespace
GameplayKillResult invest_gameplay_kill(const bounties::gameplay::Event& event,
                                        const activity::SessionBinding& owner,
                                        std::uint64_t playerKey,
                                        std::uint32_t reorderSequence) noexcept {
    namespace db = investment::store;
    // SQLite allocates one durable process epoch. Retries reuse it only after its first commit.
    static std::uint64_t epoch{};
    if (!playerKey || !reorderSequence || !activity::binding_matches(owner)
        || !activity::is_joined(owner.sessionId))
        return GameplayKillResult{GameplayKillStatus::invalidContext};
    db::Transaction transaction;
    if (!transaction.ready()) return GameplayKillResult{GameplayKillStatus::invalidAccount};
    auto candidateEpoch = epoch;
    if (!candidateEpoch) {
        if (!db::execute("INSERT INTO gameplay_runs DEFAULT VALUES"))
            return GameplayKillResult{GameplayKillStatus::invalidAccount};
        candidateEpoch = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db::g_database));
    }
    db::Statement prior("SELECT 1 FROM gameplay_receipts WHERE epoch=? AND session=? AND "
                        "revision=? AND player=? AND sequence=?");
    if (!prior.parameters(
            candidateEpoch, owner.sessionId, owner.createdRevision, playerKey, reorderSequence))
        return GameplayKillResult{GameplayKillStatus::invalidAccount};
    const int found = prior.step();
    if (found == SQLITE_ROW) return GameplayKillResult{GameplayKillStatus::duplicate};
    if (found != SQLITE_DONE) return GameplayKillResult{GameplayKillStatus::invalidAccount};
    const auto snapshot = std::unique_ptr<AccountState>(new (std::nothrow) AccountState);
    if (!snapshot || !db::read_account(*snapshot))
        return GameplayKillResult{GameplayKillStatus::invalidAccount};
    const auto context = bounties::gameplay::Context{
        owner.sessionId, owner.createdRevision, event.context.sourceGeneration};
    auto result = bounties::detail::apply_gameplay_kill_transaction(
        *snapshot,
        event,
        context,
        static_cast<std::uint16_t>(owner.destination.activityIndex),
        bounties::kKillRules,
        core::runtime::investment_clock_seconds(),
        &mapping_valid,
        &account::valid);
    if (result.status != GameplayKillStatus::applied
        && result.status != GameplayKillStatus::acceptedNoChange)
        return result;
    // Reviewed Tripmine/Hunter source identity. Other named ability sources remain unmapped.
    if (event.grenadeKill == true && event.abilityLabelHash == 0xD2180725U
        && event.playerClassHash == 0x2809035FU) {
        using Advance = unlocks::records::ObjectiveAdvance;
        const auto advanced = unlocks::records::advance_interval_objective(422U, 0x2FC2310AU);
        result.triumphsAdvanced = advanced == Advance::advanced || advanced == Advance::completed;
    }
    db::Statement receipt("INSERT INTO gameplay_receipts VALUES(?,?,?,?,?)");
    if (!receipt.write(
            candidateEpoch, owner.sessionId, owner.createdRevision, playerKey, reorderSequence)
        || (result.changedCount && !db::write_account(*snapshot)) || !transaction.commit())
        return GameplayKillResult{GameplayKillStatus::invalidAccount};
    epoch = candidateEpoch;
    return result;
}
} // namespace sunrise::state
