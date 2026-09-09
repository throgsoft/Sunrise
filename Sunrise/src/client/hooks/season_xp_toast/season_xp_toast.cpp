#include "season_xp_toast.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/progressions/progression_catalog.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "paired_toast.h"

namespace sunrise::client::hooks::season_xp_toast {
namespace {

// Original build SHA-256 81964380664e7fcee3c620085a157fdeaf91fefacf7214907820f188bbeb4ced.
// Each pattern is unique across both captured executable sections. No RVA fallback.
constexpr std::string_view kDiffText = "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 8B F1 48 "
                                       "89 7C 24 30 48 2B F2 48 8D 5A 0C BD 7F 00 00 00";
constexpr std::string_view kEnqueueText =
    "48 83 EC 38 8B 02 41 B9 04 00 00 00 48 8D 54 24 48 C6 44 24 28 00 89 44 24 48 C6 44 24 20 00";
constexpr std::string_view kAccountText =
    "48 8D 95 3C 6C 00 00 49 8D 8E 3C 6C 00 00 E8 ? ? ? ? 4C 8D 85 08 11 00 00";
constexpr std::string_view kProducerText =
    "8B 43 20 89 44 24 4C E8 ? ? ? ? 48 8B C8 4C 8D 44 24 60 48 8D 54 24 4C E8 ? ? ? ?";
constexpr auto kDiff = patterns::signature<patterns::signature_length(kDiffText)>(kDiffText);
constexpr auto kEnqueue =
    patterns::signature<patterns::signature_length(kEnqueueText)>(kEnqueueText);
constexpr auto kAccount =
    patterns::signature<patterns::signature_length(kAccountText)>(kAccountText);
constexpr auto kProducer =
    patterns::signature<patterns::signature_length(kProducerText)>(kProducerText);

using Diff = void(__fastcall*)(const detail::Entry*, const detail::Entry*);
// 1314390 leaves the core's EAX queue result intact, despite Ghidra's inferred void signature.
using Enqueue = std::int32_t(__fastcall*)(void*, const std::uint32_t*, const void*);
std::array<hooking::detour::Handle, 2> g_hooks{};
SRWLOCK g_lifecycle = SRWLOCK_INIT;
std::atomic_bool g_ready{false};
std::atomic_uint32_t g_active{0};
std::atomic_uint32_t g_suppressed{0};
const void* g_accountReturn{};
const void* g_producerReturn{};
const void* g_diffProducerReturn{};

struct Scope {
    detail::Pair pair{};
    void* manager{};
    bool canonicalAttempted{};
};
thread_local Scope* g_scope{};

// These payload calculations apply only to the installed zero-first-step pass and repeating
// single-step HUD ladder. A different content build must keep its native presentation.
bool compatible_ladders() noexcept {
    namespace catalog = state::build_data::progressions;
    std::array<catalog::Step, catalog::kStepPerDefinitionCapacity> steps{};
    std::size_t count{};
    if (!catalog::steps(40, steps, count) || count != 100 || steps[0].cost != 0) return false;
    for (std::size_t i = 1; i < count; ++i) {
        if (steps[i].cost != detail::kRankXp) return false;
    }
    return catalog::steps(41, steps, count) && count == 1 && steps[0].cost == detail::kRankXp;
}

// Separate trivial SEH helpers: faults in observation pass through to the native implementation.
bool copy_banks(const detail::Entry* before,
                const detail::Entry* after,
                detail::Entry* b,
                detail::Entry* a) noexcept {
    __try {
        std::memcpy(b, before, 127 * sizeof(*b));
        std::memcpy(a, after, 127 * sizeof(*a));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_toast(const std::uint32_t* hash,
                const void* payload,
                std::uint32_t& value,
                detail::Progress& progress) noexcept {
    __try {
        value = *hash;
        std::memcpy(&progress, static_cast<const std::byte*>(payload) + 0x910, sizeof(progress));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool canonical_queued(void* manager, const detail::Progress& expected) noexcept {
    __try {
        const auto address = reinterpret_cast<std::uintptr_t>(manager);
        const auto count = *reinterpret_cast<const std::uint64_t*>(address + 0x9710);
        if (count > 16) return false;
        const auto records = (address + 15) & ~std::uintptr_t{15};
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto row = records + i * 0x970;
            if (*reinterpret_cast<const std::uint32_t*>(row + 0x940) != detail::kPassToast)
                continue;
            detail::Progress progress{};
            std::memcpy(&progress, reinterpret_cast<const void*>(row + 0x910), sizeof(progress));
            if (detail::covers(progress, expected)) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Require E07510's immediate caller to be this bank diff. Nested item/HUD callbacks do not qualify.
bool direct_bank_producer() noexcept {
    void* frames[12]{};
    const auto count = CaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT i = 0; i + 1 < count; ++i) {
        if (frames[i] == g_producerReturn) return frames[i + 1] == g_diffProducerReturn;
    }
    return false;
}

void invoke_diff(const detail::Entry* before, const detail::Entry* after, Scope* scope) {
    Scope* previous = g_scope;
    g_scope = scope; // Nested/unrelated diffs mask, then restore the outer update.
    __try {
        reinterpret_cast<Diff>(g_hooks[0].original)(before, after);
    } __finally {
        g_scope = previous;
    }
}

void observe_diff(const detail::Entry* before, const detail::Entry* after, const void* caller) {
    Scope scope{};
    std::array<detail::Entry, 127> b{}, a{};
    if (caller == g_accountReturn && before && after
        && copy_banks(before, after, b.data(), a.data())) {
        scope.pair = detail::paired_update(b, a);
        if (scope.pair.eligible && !compatible_ladders()) scope.pair = {};
    }
    invoke_diff(before, after, scope.pair.eligible ? &scope : nullptr);
}

std::int32_t
observe_enqueue(void* manager, const std::uint32_t* hash, const void* payload, const void* caller) {
    Scope* scope = g_scope;
    std::uint32_t value{};
    detail::Progress progress{};
    const bool candidate = scope && scope->pair.eligible && caller == g_producerReturn && manager
                           && hash && payload && direct_bank_producer()
                           && read_toast(hash, payload, value, progress);
    if (candidate && value == detail::kPrestigeToast && progress == scope->pair.prestige
        && scope->canonicalAttempted && scope->manager == manager
        && canonical_queued(manager, scope->pair.pass)) {
        scope->pair.eligible = false; // At most one suppressed toast per paired update.
        const auto count = g_suppressed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 16 || (count & (count - 1)) == 0) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "ev=season_xp_toast result=duplicate_suppressed count=%u before=%d "
                              "after=%d rank=%d",
                              count,
                              progress.beforeXp,
                              progress.afterXp,
                              scope->pair.pass.afterRank);
        }
        return 0; // Same result as native coalescing; E07510 does not consume it.
    }
    const auto result = reinterpret_cast<Enqueue>(g_hooks[1].original)(manager, hash, payload);
    if (candidate && value == detail::kPassToast && progress == scope->pair.pass) {
        scope->canonicalAttempted = true;
        scope->manager = manager;
    }
    return result;
}

__declspec(noinline) void __fastcall diff(const detail::Entry* before, const detail::Entry* after) {
    g_active.fetch_add(1, std::memory_order_acq_rel);
    __try {
        // Detours resumes other threads before its wrapper publishes the trampoline handles.
        while (!g_ready.load(std::memory_order_acquire))
            YieldProcessor();
        observe_diff(before, after, _ReturnAddress());
    } __finally {
        g_active.fetch_sub(1, std::memory_order_acq_rel);
    }
}

__declspec(noinline) std::int32_t __fastcall
enqueue(void* manager, const std::uint32_t* hash, const void* payload) {
    g_active.fetch_add(1, std::memory_order_acq_rel);
    __try {
        while (!g_ready.load(std::memory_order_acquire))
            YieldProcessor();
        return observe_enqueue(manager, hash, payload, _ReturnAddress());
    } __finally {
        g_active.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool idle() noexcept {
    return g_active.load(std::memory_order_acquire) == 0;
}

bool resolve_sites(std::byte* diffTarget,
                   std::byte* enqueueTarget,
                   std::byte* account,
                   std::byte* producer) noexcept {
    if (!diffTarget || !enqueueTarget || !account || !producer) return false;
    __try {
        // E07959 -> E07AF0; E076BB -> 1314390; E07B5B -> E07510.
        if (patterns::resolve_relative(account + 15, account + 19) != diffTarget
            || patterns::resolve_relative(producer + 26, producer + 30) != enqueueTarget
            || diffTarget[0x6B] != std::byte{0xE8}
            || patterns::resolve_relative(diffTarget + 0x6C, diffTarget + 0x70) != producer - 0x192)
            return false;
        g_accountReturn = account + 19;
        g_producerReturn = producer + 30;
        g_diffProducerReturn = diffTarget + 0x70;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_hooks[0].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    auto* d = patterns::scan_main_image_unique(kDiff, "season_xp_account_diff");
    auto* e = patterns::scan_main_image_unique(kEnqueue, "season_xp_toast_enqueue");
    auto* a = patterns::scan_main_image_unique(kAccount, "season_xp_account_caller");
    auto* p = patterns::scan_main_image_unique(kProducer, "season_xp_toast_producer");
    const std::array<hooking::detour::Spec, 2> specs{{
        {d, reinterpret_cast<void*>(&diff)},
        {e, reinterpret_cast<void*>(&enqueue)},
    }};
    const bool installed = resolve_sites(d, e, a, p) && hooking::detour::install(specs, g_hooks);
    if (installed) g_ready.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     installed ? "ev=season_xp_toast result=installed presentation_only=1"
                               : "ev=season_xp_toast result=unavailable native_passthrough=1");
    return installed;
}

bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    const std::array<hooking::detour::ProtectedCodeEntry, 2> entries{{
        {reinterpret_cast<void*>(&diff)},
        {reinterpret_cast<void*>(&enqueue)},
    }};
    if (g_hooks[0].attached
        && hooking::detour::uninstall(g_hooks, entries, &idle)
               != hooking::detour::UninstallResult::removed) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return false;
    }
    g_ready.store(false, std::memory_order_release);
    g_accountReturn = g_producerReturn = g_diffProducerReturn = nullptr;
    ReleaseSRWLockExclusive(&g_lifecycle);
    return true;
}

} // namespace sunrise::client::hooks::season_xp_toast
