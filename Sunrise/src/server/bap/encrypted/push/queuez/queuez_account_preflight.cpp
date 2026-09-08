#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/runtime/dawning_default_oven_runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/** Set once the verdict can no longer change, so later frames skip the state lock. */
std::atomic<bool> g_settled{false};
/** Diagnostic suppression only; the durable per-character marker decides whether to grant. */
std::atomic<state::DawningOvenBootstrapStatus> g_ovenStatus{
    state::DawningOvenBootstrapStatus::notReady};

/**
 * Reports one preflight that left the account uncanonical.
 * @param level Level to report at.
 * @param reason Short skip reason written to the line.
 */
void report(core::log::Level level, const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=queuez stage=account_preflight result=skip reason=%s",
                                      reason);
    if (written > 0) {
        core::log::write(
            core::log::Channel::server, level, {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Canonicalizes the account before any family image is allowed to read it. */
void ensure_account_canonical() noexcept {
    // Seed all character slots before the initial snapshot, even before a character is picked.
    // Durable per-character markers preserve later discards across requests and restarts.
    const auto oven = state::ensure_default_dawning_oven();
    const auto previous = g_ovenStatus.exchange(oven.status, std::memory_order_relaxed);
    if (oven.status == state::DawningOvenBootstrapStatus::refused && previous != oven.status) {
        report(core::log::Level::warn, "default_oven_refused");
    }
    if (oven.changed) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=default_oven result=granted");
    }
    // The State call has released SQLite, and the upcoming snapshot reads the committed oven.
    // Do not reacquire the session lock through the public resync wrapper from this preflight.
    if (g_settled.load(std::memory_order_acquire)) {
        return;
    }
    switch (state::ensure_character_emote_collection()) {
    case state::EmoteCollectionOutcome::ready:
        g_settled.store(true, std::memory_order_release);
        break;
    case state::EmoteCollectionOutcome::unsupported:
        // The installed content decides this and cannot change under a running process.
        g_settled.store(true, std::memory_order_release);
        report(core::log::Level::warn, "unsupported");
        break;
    case state::EmoteCollectionOutcome::notReady:
        // Normal until content extraction and account setup finish; every family still reads the
        // same un-migrated account, so the images agree with each other.
        report(core::log::Level::debug, "not_ready");
        break;
    case state::EmoteCollectionOutcome::failed:
        report(core::log::Level::warn, "failed");
        break;
    }
}

} // namespace sunrise::server::bap::encrypted::push
