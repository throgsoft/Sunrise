#include "investment_derived_rebuild.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../hooking/detour.h"
#include "../../../memory/current_process_memory.h"
#include "derived_cache_refresh.h"
#include "internal.h"
#include "oven_received_state.h"

namespace sunrise::client::hooks::network::investment {
namespace {

/**
 * The derived-state freshness verdict. Only a rebuild recomputes the expiry, so a character
 * cached before its replicated objects arrive never goes stale by itself.
 */
constexpr std::string_view kFreshnessSignatureText =
    "48 89 5C 24 ? 57 48 83 EC ? 48 8B D9 E8 ? ? ? ? 48 8B F8 0F B6 40 08 84 C0 74 ? 48 8B 53 18 "
    "48 8B 4B 08 E8 ? ? ? ?";
/** Compiled pattern bytes for the freshness verdict above. */
constexpr auto kFreshnessSignature =
    signature<signature_length(kFreshnessSignatureText)>(kFreshnessSignatureText);

/** The state-three family-four lookup call. A nonnull result proves the real object arrived. */
constexpr std::string_view kFamily4CallSignatureText =
    "48 8D 4B 10 E8 ? ? ? ? 48 8D B8 28 07 00 00 48 83 3F 00";
/** Compiled pattern bytes for the family-four lookup call above. */
constexpr auto kFamily4CallSignature =
    signature<signature_length(kFamily4CallSignatureText)>(kFamily4CallSignatureText);

// Native 50A260 can return a valid derived bank without consulting freshness. Its account
// interface's +F0 method only returns the cached bank; the original accessor owns rebuilding.
constexpr std::string_view kDerivedAccessText =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B D9 FF 90 F0 00 00 00 "
    "48 8B F8 80 38 00 0F 84 ? ? ? ? 80 78 02 00";
constexpr auto kDerivedAccessSignature =
    signature<signature_length(kDerivedAccessText)>(kDerivedAccessText);
// Native 50BBE0 obtains the mutable bank through +F8, advances its +15760 revision when
// valid, then clears validity. Use this operation rather than writing native derived flags.
constexpr std::string_view kInvalidateText = "48 83 EC 28 48 8B 01 FF 90 F8 00 00 00 80 38 00 "
                                             "74 09 FF 80 60 57 01 00 C6 00 00 48 83 C4 28 C3";
constexpr auto kInvalidateSignature = signature<signature_length(kInvalidateText)>(kInvalidateText);

/** Byte offset of the `E8` near call inside the matched family-four pattern. */
constexpr std::size_t kFamily4CallOperandOffset = 5;
/** An x64 near call has a 4-byte relative displacement. */
constexpr std::size_t kNearCallOperandSize = 4;
/** Required detour pair followed by the optional native cache accessor. */
constexpr std::size_t kFreshnessHandle = 0;
constexpr std::size_t kFamily4Handle = 1;
constexpr std::size_t kDerivedAccessHandle = 2;
/** The freshness verdict the game reads as "rebuild required". */
constexpr char kStale = 0;

using Freshness = char(__fastcall*)(void*);
using Family4Lookup = void*(__fastcall*)(std::uint64_t*);
using DerivedAccess = void*(__fastcall*)(void*);
using NativeInvalidate = void(__fastcall*)(void*);

std::array<hooking::detour::Handle, 3> g_handles{};
std::atomic<Freshness> g_originalFreshness{nullptr};
std::atomic<Family4Lookup> g_originalFamily4Lookup{nullptr};
std::atomic_bool g_rebuildArmed{false};
std::atomic<void*> g_committedFamily4{nullptr};
std::atomic<DerivedAccess> g_originalDerivedAccess{nullptr};
std::atomic<NativeInvalidate> g_nativeInvalidate{nullptr};
std::atomic_uint64_t g_cacheRevision{0}, g_nextReceivedPoll{0};
std::atomic_uint32_t g_refreshReports{0};
std::atomic_bool g_cacheFaulted{false};
std::atomic_bool g_primaryReady{false};
std::atomic_uint32_t g_activeCalls{0};
SRWLOCK g_receivedLock = SRWLOCK_INIT, g_cacheLock = SRWLOCK_INIT;
oven_received::Tracker g_received{};
oven_received::Inputs g_receivedScratch{}; // Two bounded snapshots, never on the game's stack.
derived_refresh::Ledger g_cacheRefresh{};

template <class T> bool read_native(std::uintptr_t address, T& value) noexcept {
    return memory::read_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&value, 1)));
}

bool claim_refresh_report() noexcept {
    auto count = g_refreshReports.load(std::memory_order_relaxed);
    while (count < 128) {
        if (g_refreshReports.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

// The extra native calls are guarded independently; the original accessor remains authoritative.
void* cached_bank(DerivedAccess getter, void* account) noexcept {
    __try {
        return getter(account);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cacheFaulted.store(true, std::memory_order_release);
        return nullptr;
    }
}

bool invalidate_bank(NativeInvalidate invalidate, void* account) noexcept {
    __try {
        invalidate(account);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cacheFaulted.store(true, std::memory_order_release);
        return false;
    }
}

void observe_received(void* resolved) noexcept {
    const auto now = GetTickCount64();
    auto next = g_nextReceivedPoll.load(std::memory_order_acquire);
    if (now < next || !g_nextReceivedPoll.compare_exchange_strong(next, now + 16)
        || !TryAcquireSRWLockExclusive(&g_receivedLock))
        return;
    const bool readable = oven_received::read_inputs(
        reinterpret_cast<std::uintptr_t>(resolved),
        [](std::uintptr_t address, std::span<std::byte> output) {
            return memory::read_current_process(nullptr, address, output);
        },
        g_receivedScratch);
    const bool changed = readable && g_received.observe(g_receivedScratch);
    const auto revision = changed ? g_cacheRevision.fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
    ReleaseSRWLockExclusive(&g_receivedLock);
    if (changed) {
        arm_derived_rebuild();
        if (claim_refresh_report())
            core::log::writef(core::log::Channel::client,
                              core::log::Level::debug,
                              "ev=dawning_derived stage=received result=changed revision=%llu",
                              static_cast<unsigned long long>(revision));
    }
}

// Refresh each of at most 64 native banks once per observed revision. Contention/reentrancy
// skips this attempt; failed preflight never consumes the revision. Retain no borrowed bank.
void* derived_access_impl(void* account) noexcept {
    const auto original = g_originalDerivedAccess.load(std::memory_order_acquire);
    const auto invalidate = g_nativeInvalidate.load(std::memory_order_acquire);
    const auto revision = g_cacheRevision.load(std::memory_order_acquire);
    auto result = derived_refresh::Result::unchanged;
    if (account && invalidate && revision && !g_cacheFaulted.load(std::memory_order_acquire)) {
        std::uintptr_t table{};
        DerivedAccess getter{};
        if (read_native(reinterpret_cast<std::uintptr_t>(account), table) && table
            && table <= (std::numeric_limits<std::uintptr_t>::max)() - 0xF0
            && read_native(table + 0xF0, getter) && getter) {
            void* const bank = cached_bank(getter, account);
            if (bank && TryAcquireSRWLockExclusive(&g_cacheLock)) {
                result = g_cacheRefresh.apply(
                    reinterpret_cast<std::uintptr_t>(bank), revision, [&](std::uintptr_t address) {
                        std::uint8_t valid{};
                        std::uint32_t before{}, after{};
                        if (address > (std::numeric_limits<std::uintptr_t>::max)() - 0x15764
                            || !read_native(address, valid) || valid > 1
                            || !read_native(address + 0x15760, before))
                            return false;
                        if (!valid) return true; // Already owes the original accessor a rebuild.
                        if (!invalidate_bank(invalidate, account)) return false;
                        if (read_native(address, valid) && !valid
                            && read_native(address + 0x15760, after) && after == before + 1U)
                            return true;
                        // An uncertain native call may have partly executed. Do not repeat it.
                        g_cacheFaulted.store(true, std::memory_order_release);
                        return false;
                    });
                ReleaseSRWLockExclusive(&g_cacheLock);
            }
        }
    }
    if ((result != derived_refresh::Result::unchanged || g_cacheFaulted.load())
        && claim_refresh_report())
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=dawning_derived stage=cache result=%s revision=%llu quarantined=%u",
                          result == derived_refresh::Result::invalidated ? "native_invalidated"
                          : result == derived_refresh::Result::full      ? "tracking_full"
                                                                         : "refused",
                          static_cast<unsigned long long>(revision),
                          unsigned(g_cacheFaulted.load()));
    return original ? original(account) : nullptr;
}

/** @return True while any primary rebuild detour is attached. */
[[nodiscard]] bool any_primary_attached() noexcept {
    return g_handles[kFreshnessHandle].attached || g_handles[kFamily4Handle].attached
           || g_handles[kDerivedAccessHandle].attached;
}

/** Clears call targets and the pending arm after full detach. */
void clear_runtime() noexcept {
    g_primaryReady.store(false, std::memory_order_release);
    g_originalFreshness.store(nullptr, std::memory_order_release);
    g_originalFamily4Lookup.store(nullptr, std::memory_order_release);
    g_rebuildArmed.store(false, std::memory_order_release);
    g_committedFamily4.store(nullptr, std::memory_order_release);
    g_originalDerivedAccess.store(nullptr, std::memory_order_release);
    g_nativeInvalidate.store(nullptr, std::memory_order_release);
    g_cacheRevision.store(0, std::memory_order_release);
    g_nextReceivedPoll.store(0, std::memory_order_release);
    g_refreshReports.store(0, std::memory_order_release);
    g_cacheFaulted.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_receivedLock);
    g_received.initialized = false;
    ReleaseSRWLockExclusive(&g_receivedLock);
    AcquireSRWLockExclusive(&g_cacheLock);
    g_cacheRefresh = {};
    ReleaseSRWLockExclusive(&g_cacheLock);
}

/**
 * Reports stale once after a real replicated-object commit.
 * @param accessor Borrowed derived-state accessor.
 * @return Stale once while armed, otherwise the native verdict.
 */
char freshness_impl(void* accessor) noexcept {
    const Freshness original = g_originalFreshness.load(std::memory_order_acquire);
    // The native verdict runs the Family-4 lookup that arms the first rebuild, so call it before
    // consuming the arm.
    const char nativeVerdict = original != nullptr ? original(accessor) : kStale;
    if (g_rebuildArmed.exchange(false, std::memory_order_acq_rel)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=investment stage=derived result=rebuilt");
        return kStale;
    }
    return nativeVerdict;
}

/**
 * Arms a rebuild when the state-three lookup returns a different committed Family-4 object.
 * Arm on identity change, never on nonnull: the freshness verdict runs this lookup itself.
 * @param key Borrowed account key.
 * @return The native lookup result, unchanged.
 */
void* family4_lookup_impl(std::uint64_t* key) noexcept {
    const Family4Lookup original = g_originalFamily4Lookup.load(std::memory_order_acquire);
    void* const resolved = original != nullptr ? original(key) : nullptr;
    if (resolved != nullptr) observe_received(resolved);
    void* previous = g_committedFamily4.load(std::memory_order_acquire);
    if (resolved != nullptr && resolved != previous
        && g_committedFamily4.compare_exchange_strong(
            previous, resolved, std::memory_order_acq_rel, std::memory_order_acquire)) {
        arm_derived_rebuild();
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=investment stage=family4_commit result=armed");
    }
    return resolved;
}

// Publish trampolines before callbacks use them, and retain ownership across native calls.
// The finally blocks balance activity even when a native exception crosses the wrapper.
__declspec(noinline) char __fastcall freshness(void* accessor) noexcept {
    g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
    __try {
        while (!g_primaryReady.load(std::memory_order_acquire))
            YieldProcessor();
        return freshness_impl(accessor);
    } __finally {
        g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
}

__declspec(noinline) void* __fastcall family4_lookup(std::uint64_t* key) noexcept {
    g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
    __try {
        while (!g_primaryReady.load(std::memory_order_acquire))
            YieldProcessor();
        return family4_lookup_impl(key);
    } __finally {
        g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
}

__declspec(noinline) void* __fastcall derived_access(void* account) noexcept {
    g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
    __try {
        while (!g_primaryReady.load(std::memory_order_acquire))
            YieldProcessor();
        return derived_access_impl(account);
    } __finally {
        g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool primary_idle() noexcept {
    return g_activeCalls.load(std::memory_order_acquire) == 0;
}

bool detach_primary() noexcept {
    const std::array<hooking::detour::ProtectedCodeEntry, 3> entries{{
        {reinterpret_cast<void*>(&freshness)},
        {reinterpret_cast<void*>(&family4_lookup)},
        {reinterpret_cast<void*>(&derived_access)},
    }};
    const std::size_t count = g_handles[kDerivedAccessHandle].attached ? 3 : 2;
    return hooking::detour::uninstall(
               std::span(g_handles).first(count), std::span(entries).first(count), &primary_idle)
           == hooking::detour::UninstallResult::removed;
}

} // namespace

/** Arms one derived-state rebuild, used up by the next freshness verdict. */
void arm_derived_rebuild() noexcept {
    g_rebuildArmed.store(true, std::memory_order_release);
}

/** Arms the rebuild on a committed publication. Repeat publications reuse the one arm. */
void notify_investment_publication() noexcept {
    arm_derived_rebuild();
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=investment stage=publication result=armed");
}

/** Definition expressions changed in place; cached conditions must be recomputed too. */
void notify_investment_definition_change() noexcept {
    g_cacheRevision.fetch_add(1, std::memory_order_acq_rel);
    arm_derived_rebuild();
}

/** @return True when freshness and both real-arrival rebuild arms are attached. */
bool install() noexcept {
    if (is_installed()) {
        return true;
    }
    if (has_ownership()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=install result=fail reason=ownership");
        return false;
    }

    std::byte* const freshnessTarget =
        scan_main_image_unique(kFreshnessSignature, "investment_derived_freshness");
    std::byte* const family4Call =
        scan_main_image_unique(kFamily4CallSignature, "queuez_family4_readiness_call");
    std::byte* const derivedAccessTarget =
        scan_main_image_unique(kDerivedAccessSignature, "investment_derived_access");
    std::byte* const invalidateTarget =
        scan_main_image_unique(kInvalidateSignature, "investment_native_invalidate");
    if (freshnessTarget == nullptr || family4Call == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=install result=fail reason=target");
        return false;
    }
    std::byte* const family4Target =
        resolve_relative(family4Call + kFamily4CallOperandOffset,
                         family4Call + kFamily4CallOperandOffset + kNearCallOperandSize);
    if (family4Target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=install result=fail reason=operand");
        return false;
    }

    const std::array specs{
        hooking::detour::Spec{freshnessTarget, reinterpret_cast<void*>(&freshness)},
        hooking::detour::Spec{family4Target, reinterpret_cast<void*>(&family4_lookup)},
        hooking::detour::Spec{derivedAccessTarget, reinterpret_cast<void*>(&derived_access)},
    };
    std::array<hooking::detour::Handle, 3> installed{};
    std::size_t count = derivedAccessTarget != nullptr && invalidateTarget != nullptr ? 3 : 2;
    bool attached =
        hooking::detour::install(std::span(specs).first(count), std::span(installed).first(count));
    if (!attached && count == 3) {
        // A failed detour transaction leaves the targets unchanged. Preserve the required pair
        // when this client's optional cache accessor cannot be attached.
        installed = {};
        count = 2;
        attached = hooking::detour::install(std::span(specs).first(count),
                                            std::span(installed).first(count));
    }
    if (!attached) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=install result=fail reason=attach");
        return false;
    }
    g_handles = installed;
    g_originalFreshness.store(reinterpret_cast<Freshness>(g_handles[kFreshnessHandle].original),
                              std::memory_order_release);
    g_originalFamily4Lookup.store(
        reinterpret_cast<Family4Lookup>(g_handles[kFamily4Handle].original),
        std::memory_order_release);
    g_originalDerivedAccess.store(
        reinterpret_cast<DerivedAccess>(g_handles[kDerivedAccessHandle].original),
        std::memory_order_release);
    g_nativeInvalidate.store(count == 3 ? reinterpret_cast<NativeInvalidate>(invalidateTarget)
                                        : nullptr,
                             std::memory_order_release);
    g_primaryReady.store(true, std::memory_order_release);
    if (count == 2)
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=cache_refresh result=unavailable");

    if (!install_family5_rearm()) {
        if (detach_primary()) {
            clear_runtime();
        } else {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=investment stage=install result=fail reason=rollback");
        }
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=investment stage=install result=ok");
    return true;
}

/** @return True when every investment rebuild detour is absent. */
bool uninstall() noexcept {
    restore_lore_visibility();
    restore_socket_menu_routing();
    if (!uninstall_family5_rearm()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=uninstall result=fail reason=family5");
        return false;
    }
    if (any_primary_attached()
        && (!g_handles[kFreshnessHandle].attached || !g_handles[kFamily4Handle].attached
            || !detach_primary())) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=uninstall result=fail reason=detach");
        return false;
    }
    clear_runtime();
    return true;
}

/** @return True while freshness and both real-arrival rebuild arms are attached. */
bool is_installed() noexcept {
    return g_handles[kFreshnessHandle].attached && g_handles[kFamily4Handle].attached
           && family5_rearm_is_installed();
}

/** @return True while any investment rebuild detour still needs cleanup. */
bool has_ownership() noexcept {
    return any_primary_attached() || family5_rearm_is_installed();
}

} // namespace sunrise::client::hooks::network::investment
