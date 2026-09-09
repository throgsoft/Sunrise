#include "material_notifications.h"

#include <algorithm>
#include <array>
#include <mutex>

#include "../../../core/logging/log.h"

namespace sunrise::server::bap::presentation::material_notifications {
namespace {
struct Pending {
    Notice notice{};
    std::uint64_t created{};
};
std::mutex g_lock;
std::array<Pending, kCapacity> g_pending{};
std::size_t g_head{}, g_count{};
std::uint64_t g_generation{};
unsigned g_overflowLogs{}; // Process lifetime, deliberately not reset by clear.
bool g_servicing{};
thread_local CommitScope* g_commitScope{};

bool valid_gains(std::uint64_t accountSoid,
                 std::uint64_t characterSoid,
                 std::span<const std::int32_t> gains) noexcept {
    return accountSoid != 0 && characterSoid != 0 && gains.size() == kIngredientCount
           && std::none_of(gains.begin(), gains.end(), [](auto gain) { return gain < 0; });
}

bool valid(const Notice& notice) noexcept {
    return notice.accountSoid != 0 && notice.characterSoid != 0
           && notice.ingredientOrdinal < kIngredientCount && notice.quantity > 0;
}
// Caller holds g_lock. A retry cannot displace a newer committed acquisition.
bool append(const Pending& pending) noexcept {
    if (g_count == kCapacity) return false;
    g_pending[(g_head + g_count) % kCapacity] = pending;
    ++g_count;
    return true;
}
bool report_overflow_locked() noexcept {
    if (g_overflowLogs == kOverflowLogLimit) return false;
    ++g_overflowLogs;
    return true;
}
void report_overflow(bool report, const char* stage) noexcept {
    if (report)
        core::log::writef(
            core::log::Channel::server,
            core::log::Level::warn,
            "ev=material_toast stage=%s result=queue_full capacity=64 process_log_limit=8",
            stage);
}
} // namespace

EnqueueResult enqueue(const Notice& notice, std::uint64_t now) noexcept {
    if (!valid(notice)) return EnqueueResult::invalid;
    bool report = false;
    {
        const std::lock_guard lock(g_lock);
        if (append({notice, now})) return EnqueueResult::enqueued;
        report = report_overflow_locked();
    }
    report_overflow(report, "enqueue");
    return EnqueueResult::full;
}

GainsResult enqueue_committed_gains(std::uint64_t accountSoid,
                                    std::uint64_t characterSoid,
                                    std::span<const std::int32_t> gains,
                                    std::uint64_t now) noexcept {
    GainsResult result{};
    if (!valid_gains(accountSoid, characterSoid, gains)) return result;
    result.valid = true;
    for (std::size_t i = 0; i < gains.size(); ++i) {
        if (gains[i] == 0) continue;
        const auto queued =
            enqueue({accountSoid, characterSoid, static_cast<std::uint8_t>(i), gains[i]}, now);
        if (queued == EnqueueResult::enqueued)
            ++result.enqueued;
        else
            ++result.full;
    }
    return result;
}

CommitScope::CommitScope() noexcept : previous_(g_commitScope) {
    g_commitScope = this;
}

CommitScope::~CommitScope() {
    g_commitScope = previous_;
}

bool stage_committed_gains(std::uint64_t accountSoid,
                           std::uint64_t characterSoid,
                           std::span<const std::int32_t> gains) noexcept {
    auto* scope = g_commitScope;
    if (!scope || scope->previous_ || scope->staged_
        || !valid_gains(accountSoid, characterSoid, gains))
        return false;
    scope->accountSoid_ = accountSoid;
    scope->characterSoid_ = characterSoid;
    std::copy(gains.begin(), gains.end(), scope->gains_.begin());
    scope->staged_ = true;
    return true;
}

GainsResult CommitScope::publish(std::uint64_t now) noexcept {
    if (g_commitScope != this || previous_ || !staged_) return {};
    staged_ = false;
    return enqueue_committed_gains(accountSoid_, characterSoid_, gains_, now);
}

void clear() noexcept {
    const std::lock_guard lock(g_lock);
    g_pending = {};
    g_head = g_count = 0;
    ++g_generation;
    // Keep servicing latched until any callback returns. In particular, a callback that clears
    // and then calls service recursively must not start a second drain on a partially unwound UI.
}

ServiceResult service(std::uint64_t now, Consumer consumer, void* context) noexcept {
    ServiceResult result{};
    if (consumer == nullptr) return result;
    std::size_t budget{};
    std::uint64_t generation{};
    {
        const std::lock_guard lock(g_lock);
        if (g_servicing) return result;
        g_servicing = true;
        budget = (std::min)(g_count, kPerService);
        generation = g_generation;
    }
    for (std::size_t i = 0; i < budget; ++i) {
        Pending pending{};
        {
            const std::lock_guard lock(g_lock);
            if (generation != g_generation || g_count == 0) break;
            pending = g_pending[g_head];
            g_pending[g_head] = {};
            g_head = (g_head + 1) % kCapacity;
            --g_count;
        }
        if (now < pending.created || now - pending.created >= kLifetimeMs) {
            ++result.expired;
            continue;
        }
        ++result.attempted;
        const auto delivery = consumer(context, pending.notice);
        if (delivery == Delivery::delivered) {
            ++result.delivered;
            continue;
        }
        if (delivery != Delivery::retry) {
            ++result.discarded;
            continue;
        }
        bool report = false;
        {
            const std::lock_guard lock(g_lock);
            if (generation != g_generation)
                ++result.discarded;
            else if (append(pending))
                ++result.retried;
            else {
                ++result.discarded;
                report = report_overflow_locked();
            }
        }
        report_overflow(report, "retry");
    }
    {
        const std::lock_guard lock(g_lock);
        g_servicing = false;
    }
    return result;
}
} // namespace sunrise::server::bap::presentation::material_notifications
