#include "synthesizer_family4_refresh.h"

#include <Windows.h>

#include <algorithm>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/inventory/dawning_oven_state.h"
#include "../../../../state/runtime/chalice_crafting_runtime.h"
#include "../../../../state/runtime/synthesizer_crafting_runtime.h"
#include "../internal.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Every socketed crafting container whose slots the Client derives from received banks. */
[[nodiscard]] bool socketed_container(std::uint32_t hash) noexcept {
    return state::runtime::detail::synthesizer::is_container(hash)
           || hash == state::account::inventory::dawning::kOvenHash
           || hash == state::runtime::detail::chalice::kChaliceHash;
}

} // namespace

bool consume_synthesizer_family4_refresh(Session& session,
                                         SynthesizerFamily4Refresh& refresh,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    if (!refresh.armed || session.family5RefreshArmed || session.accountResyncArmed
        || !session.queuez.family4Active || !queuez::valid(session.queuez)
        || GetTickCount64() < refresh.dueTick)
        return false;
    // Past the due tick every pump would otherwise retry immediately, and each retry copies a
    // whole account snapshot. A transient miss waits out another settling interval instead.
    const auto retry_later = [&refresh]() noexcept {
        refresh.dueTick = GetTickCount64() + kSettlingMilliseconds;
        return false;
    };
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account) || account.primarySoid != session.queuez.family4RootSoid)
        return retry_later();
    const state::CharacterState* selected = nullptr;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        if (!account.characters[c].selected) continue;
        if (selected) return retry_later();
        selected = &account.characters[c];
    }
    if (!selected) return retry_later();
    const state::account::inventory::Item* target = nullptr;
    for (std::size_t i = 0; i < selected->inventory.count; ++i) {
        const auto& item = selected->inventory.values[i];
        if (item.instanceSoid <= refresh.lastInstanceSoid || item.quantity != 1
            || item.placement != state::account::inventory::ItemPlacement::inventory
            || !socketed_container(item.definitionHash))
            continue;
        if (!target || item.instanceSoid < target->instanceSoid) target = &item;
    }
    if (!target) {
        refresh = {};
        return false;
    }
    // Never introduce a new resident from a deferred view refresh. A resync owns that, so an
    // unmatched container disarms here rather than rescanning the account on every later pump.
    std::size_t residentMatches = 0;
    for (std::size_t i = 0; i < session.queuez.family4ResidentCount; ++i)
        residentMatches += session.queuez.family4Residents[i].objectSoid == target->instanceSoid;
    if (residentMatches != 1) {
        refresh = {};
        return false;
    }

    queuez::EquipmentSwap update{};
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    touchesScratch = true;
    // The existing artifact helper encodes any exact current item resident. Its
    // increment contains that item only: no account balances, gain ring or abilities.
    if (!queuez::stage_equipment_swap(session.queuez, selected->soid, update)
        || !push::append_artifact_item_refresh_notification(scratch,
                                                            update,
                                                            target->instanceSoid,
                                                            session.sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize)
        || framedSize == 0 || framedSize > response.size())
        return false;
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    refresh.lastInstanceSoid = target->instanceSoid;
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=crafting_visibility stage=item_refresh result=published "
                      "instance=0x%llX hash=%u family4_version=%d family5_version=%d",
                      static_cast<unsigned long long>(target->instanceSoid),
                      target->definitionHash,
                      session.queuez.family4Version,
                      session.queuez.family5Version);
    return true;
}

} // namespace sunrise::server::bap::encrypted
