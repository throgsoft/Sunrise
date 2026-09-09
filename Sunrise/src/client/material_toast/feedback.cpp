#include "feedback.h"

#include <memory>
#include <new>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../server/bap/presentation/material_notifications.h"
#include "../../state/account/inventory/dawning_oven_state.h"
#include "../../state/runtime/runtime.h"
#include "native_presentation.h"

namespace sunrise::client::material_toast {
namespace {
namespace notices = server::bap::presentation::material_notifications;
using notices::Delivery;
using notices::Notice;
static_assert(notices::kIngredientCount == state::account::inventory::dawning::kIngredients.size());
unsigned g_reports{}; // Game-thread owned, process-bounded diagnostics only.
std::string_view g_lastRefusal{};

Delivery consume(void*, const Notice& notice) noexcept {
    const std::unique_ptr<state::AccountState> selected(
        new (std::nothrow) state::AccountState(state::account_snapshot()));
    if (!selected || selected->primarySoid == 0) return Delivery::retry;
    const auto characterSoid = state::account::selected_character_soid(*selected);
    if (characterSoid == 0) return Delivery::retry;
    if (selected->primarySoid != notice.accountSoid || characterSoid != notice.characterSoid)
        return Delivery::discard;

    const auto initialized = initialize();
    const auto result = initialized.status == Status::ready
                            ? present(notice.ingredientOrdinal, notice.quantity)
                            : initialized;
    const std::string_view reason = result.reason;
    const bool delivered = result.status == Status::delivered;
    if (g_reports < 128 && (delivered || reason != g_lastRefusal)) {
        ++g_reports;
        core::log::writef(
            core::log::Channel::client,
            delivered ? core::log::Level::info : core::log::Level::warn,
            "ev=material_toast stage=present result=%s ingredient=%u quantity=%d reason=%s",
            delivered ? "queued_native" : "refused",
            static_cast<unsigned>(notice.ingredientOrdinal),
            notice.quantity,
            result.reason);
        if (!delivered) g_lastRefusal = reason;
    }
    if (delivered) return Delivery::delivered;
    // These failures precede native dispatch and can resolve on a subsequent callback.
    if (reason == "wrong_thread" || reason == "content_not_ready"
        || reason == "pickup_identity_or_content" || reason == "queue_full")
        return Delivery::retry;
    // In particular, never retry an uncertain/partially executed native enqueue.
    return Delivery::discard;
}
} // namespace

void pump(std::uint64_t now) noexcept {
    // Steam callbacks can arrive on multiple threads. Reject a foreign caller before service
    // removes/expires any notice or releases its queue slot to a concurrent producer. The
    // native adapter retains its original thread binding; only that thread may drain/log.
    if (!may_service_current_thread()) return;
    // Bind before even expiring notices: two first callbacks must not race the diagnostics.
    if (std::string_view(initialize().reason) == "wrong_thread") return;
    const auto result = notices::service(now, &consume);
    if (result.expired != 0 && g_reports < 128) {
        ++g_reports;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=material_toast stage=present result=expired count=%zu",
                          result.expired);
    }
}

void shutdown() noexcept {
    notices::clear();
}
} // namespace sunrise::client::material_toast
