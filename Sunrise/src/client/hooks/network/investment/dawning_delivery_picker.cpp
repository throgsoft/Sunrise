#include "dawning_delivery_picker.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../hooking/detour.h"
#include "../../../memory/current_process_memory.h"
#include "internal.h"
#include "investment_dawning_delivery_visibility.h"

namespace sunrise::client::hooks::network::investment {
namespace {
using Retire = bool(__fastcall*)(void*, std::uint16_t);
constexpr std::string_view kRetireText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 48 83 EC ? 44 8B 49 ? 33 ED 0F B7 DA 48 8B F1";
constexpr auto kRetire = signature<signature_length(kRetireText)>(kRetireText);
std::array<hooking::detour::Handle, 1> g_hooks{};
std::atomic_bool g_ready{};
std::atomic_uint32_t g_active{};
SRWLOCK g_lifecycle = SRWLOCK_INIT;

bool observe(void* picker, std::uint16_t interaction) noexcept {
    std::uint16_t vendor{};
    if (picker != nullptr && interaction == 21
        && memory::read_current_process(nullptr,
                                        reinterpret_cast<std::uintptr_t>(picker),
                                        std::as_writable_bytes(std::span(&vendor, 1)))
        && vendor == 16) {
        // The destination's vendor may load after the boot-time content boundary.
        // The patch additionally proves vendor/item identities and exact expression ownership.
        apply_dawning_delivery_visibility();
    }
    // Never skip an interaction, fabricate ownership or override the native picker verdict.
    return reinterpret_cast<Retire>(g_hooks[0].original)(picker, interaction);
}

__declspec(noinline) bool __fastcall retire(void* picker, std::uint16_t interaction) noexcept {
    g_active.fetch_add(1, std::memory_order_acq_rel);
    __try {
        while (!g_ready.load(std::memory_order_acquire))
            YieldProcessor();
        return observe(picker, interaction);
    } __finally {
        g_active.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool idle() noexcept {
    return g_active.load(std::memory_order_acquire) == 0;
}
} // namespace

bool install_dawning_delivery_picker() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_hooks[0].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    auto* target = scan_main_image_unique(kRetire, "dawning_delivery_picker");
    const std::array<hooking::detour::Spec, 1> specs{{
        {target, reinterpret_cast<void*>(&retire)},
    }};
    const bool installed = target != nullptr && hooking::detour::install(specs, g_hooks);
    if (installed) g_ready.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     installed ? "ev=dawning_delivery_picker result=installed event_override=1"
                               : "ev=dawning_delivery_picker result=unavailable");
    return installed;
}

bool uninstall_dawning_delivery_picker() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    const std::array<hooking::detour::ProtectedCodeEntry, 1> entries{{
        {reinterpret_cast<void*>(&retire)},
    }};
    if (g_hooks[0].attached
        && hooking::detour::uninstall(g_hooks, entries, &idle)
               != hooking::detour::UninstallResult::removed) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return false;
    }
    g_ready.store(false, std::memory_order_release);
    restore_dawning_delivery_visibility();
    ReleaseSRWLockExclusive(&g_lifecycle);
    return true;
}
} // namespace sunrise::client::hooks::network::investment
