#include "native_presentation.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../middleware/datagen/family4/inventory/layout.h"
#include "../../state/account/inventory/dawning_oven_state.h"
#include "../../state/build_data/runtime.h"
#include "../memory/current_process_memory.h"
#include "../patterns/image_scan.h"

namespace sunrise::client::material_toast {
namespace {

// Retained 81964380664e7fce. Each pattern has one match in the readable executable.
// Wrapper 1321270 calls owner getter 1353EF0 at +1E and enqueue 13776C0 at +55.
constexpr std::string_view kWrapperText =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 45 0F B6 F1 41 8B F0 "
    "8B EA 48 8B F9 E8 ? ? ? ? 80 7C 24 78 00 48 8D 58 58";
constexpr std::string_view kEnqueueText =
    "48 89 5C 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 F0 48 81 EC 10 01 00 00 "
    "48 8B D9 45 32 F6 48 8B 89 08 23 00 00 45 33 D2 45 8B F9 45 8B E0 48 8B FA 48 85 C9";
constexpr auto kWrapper =
    patterns::signature<patterns::signature_length(kWrapperText)>(kWrapperText);
constexpr auto kEnqueue =
    patterns::signature<patterns::signature_length(kEnqueueText)>(kEnqueueText);
// Explicit scope/index == -1 or instance == 0 skips the inventory-item-view constructor.
constexpr auto kNoInventory =
    patterns::signature<31>("41 83 FC FF 0F 84 B0 00 00 00 41 83 FF FF 0F 84 A6 00 00 00 "
                            "48 83 7F 08 00 0F 84 9B 00 00 00");
constexpr auto kOwnerGetter = patterns::signature<8>("48 8D 05 ? ? ? ? C3");
constexpr auto kQueueCopy =
    patterns::signature<33>("4C 8B 81 80 06 00 00 49 8D 40 01 48 89 81 80 06 00 00 48 83 C1 07 "
                            "4D 6B C0 68 48 83 E1 F8 4C 03 C1");

// Bucket 37 is the admitted native pickup route, not a persisted inventory destination.
constexpr std::uint8_t kPickupBucket = 37;
constexpr std::uint32_t kNoSourceHash = 0x811C9DC5U;
constexpr std::size_t kQueueCount = 0x1320, kQueueRows = 0xCA0, kQueueStride = 0x68;
using Item = middleware::datagen::family4::inventory::layout::Entry;
using Enqueue = void(__fastcall*)(void*, const Item*, int, int, const std::uint32_t*);
using ContentReady = char(__fastcall*)();
using ContextGetter = void*(__fastcall*)();
using CurrencyClassifier = char*(__fastcall*)(char*, std::uint16_t);
static_assert(sizeof(Item) == 32 && offsetof(Item, quantity) == 0x10);

struct Native {
    Enqueue enqueue{};
    ContentReady contentReady{};
    ContextGetter context{};
    CurrencyClassifier currency{};
    std::uintptr_t manager{};
};
Native g_native{}; // Game-thread owned; no borrowed per-frame pointers are retained.
std::atomic<DWORD> g_thread{};
bool g_attempted{}, g_resolved{}, g_quarantined{};

template <class T> bool read(std::uintptr_t at, T& value) noexcept {
    return memory::read_current_process(nullptr, at, std::as_writable_bytes(std::span(&value, 1)));
}
bool matches(std::uintptr_t at, std::span<const patterns::PatternByte> pattern) noexcept {
    if (!at || pattern.size() > 64) return false;
    std::array<std::byte, 64> bytes{};
    if (!memory::read_current_process(nullptr, at, std::span(bytes).first(pattern.size())))
        return false;
    for (std::size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i].exact && pattern[i].value != bytes[i]) return false;
    return true;
}
std::uintptr_t call_target(std::uintptr_t at) noexcept {
    std::uint8_t opcode{};
    std::int32_t displacement{};
    if (!read(at, opcode) || opcode != 0xE8 || !read(at + 1, displacement)) return 0;
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(at + 5) + displacement);
}
bool executable(std::uintptr_t at) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!at || !VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info)
        || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return (info.Protect
            & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
           != 0;
}
Result refused(const char* reason) noexcept {
    return {Status::refused, reason};
}

bool resolve_native() noexcept {
    const auto wrapper = reinterpret_cast<std::uintptr_t>(
        patterns::scan_main_image_unique(kWrapper, "dawning_pickup_wrapper"));
    const auto enqueue = reinterpret_cast<std::uintptr_t>(
        patterns::scan_main_image_unique(kEnqueue, "dawning_pickup_enqueue"));
    if (!wrapper || !enqueue || call_target(wrapper + 0x55) != enqueue
        || !matches(enqueue + 0x281, kNoInventory))
        return false;
    const auto getter = call_target(wrapper + 0x1E);
    const auto copy = call_target(enqueue + 0x347);
    if (!matches(getter, kOwnerGetter) || !matches(copy, kQueueCopy)) return false;
    std::int32_t displacement{};
    if (!read(getter + 3, displacement)) return false;
    // 1353EF0 is LEA RAX,[static owner]; RET. Resolve it without calling game code.
    const auto owner =
        static_cast<std::uintptr_t>(static_cast<std::int64_t>(getter + 7) + displacement);
    Native candidate{};
    candidate.manager = owner + 0x58;
    candidate.enqueue = reinterpret_cast<Enqueue>(enqueue);
    const auto ready = call_target(enqueue + 0xD6);
    const auto context = call_target(enqueue + 0xEB);
    const auto currency = call_target(enqueue + 0x103);
    if ((candidate.manager & 7) || !executable(ready) || !executable(context)
        || !executable(currency) || !executable(enqueue))
        return false;
    candidate.contentReady = reinterpret_cast<ContentReady>(ready);
    candidate.context = reinterpret_cast<ContextGetter>(context);
    candidate.currency = reinterpret_cast<CurrencyClassifier>(currency);
    g_native = candidate;
    return true;
}

bool exact_pickup(std::uint8_t ordinal, std::uint16_t& index) noexcept {
    namespace dawning = state::account::inventory::dawning;
    namespace data = state::build_data;
    if (ordinal >= dawning::kIngredients.size()) return false;
    const auto hash = dawning::kIngredients[ordinal].pickupHash;
    data::items::Definition pickup{}, reverse{};
    // Resolve every attempt: no retained catalog row, fixed native index or package tag.
    if (!data::find_item_definition_hash(hash, pickup) || pickup.definitionHash != hash
        || pickup.bucketId != kPickupBucket
        || pickup.definitionIndex >= data::items::kDefinitionCapacity
        || !data::find_item_definition_index(pickup.definitionIndex, reverse)
        || reverse.definitionHash != hash || reverse.bucketId != kPickupBucket)
        return false;
    index = pickup.definitionIndex;
    return true;
}

Result preflight(std::uint8_t ordinal,
                 std::uint16_t& index,
                 std::uint64_t& count,
                 std::int32_t& serial) noexcept {
    if (!g_thread.load(std::memory_order_acquire)) return refused("not_initialized");
    if (g_thread.load(std::memory_order_relaxed) != GetCurrentThreadId())
        return refused("wrong_thread");
    if (g_quarantined) return refused("quarantined");
    if (!g_resolved) return refused("signature");
    if (!exact_pickup(ordinal, index)) return refused("pickup_identity_or_content");
    if (!g_native.contentReady() || !g_native.context()) return refused("content_not_ready");
    // F29420 classifies entries routed to the separate currency queue. Refuse that path:
    // only the ordinary material queue/copy contract has been admitted by this adapter.
    char currency = -1;
    if (g_native.currency(&currency, index) != &currency || currency != -1)
        return refused("currency_route");
    std::uint64_t definitionCount{};
    if (!read(g_native.manager + kQueueCount, count) || count > 16
        || !read(g_native.manager + 0x2308, definitionCount) || definitionCount > 16
        || !read(g_native.manager + 0x1FDC, serial) || serial < 0
        || serial == (std::numeric_limits<std::int32_t>::max)())
        return refused("manager_shape");
    if (count == 16) return refused("queue_full");
    return {Status::ready, "ready"};
}

Result deliver(std::uint8_t ordinal, std::int32_t gain) noexcept {
    std::uint16_t index{};
    std::uint64_t count{};
    std::int32_t serial{};
    const auto check = preflight(ordinal, index, count, serial);
    if (check.status != Status::ready) return check;
    Item item{};
    item.definitionIndex = index;
    item.quantity = gain;
    const std::uint32_t source = kNoSourceHash;
    // No user/account object argument: manager is the static local UI owner. Scope/index
    // only locate an optional inventory item view; -1/-1 and zero instance skip it.
    // 1378260 copies all 104 notification bytes synchronously, retaining neither pointer.
    g_native.enqueue(reinterpret_cast<void*>(g_native.manager), &item, -1, -1, &source);
    std::uint64_t after{};
    std::int32_t copiedSerial{}, copiedGain{};
    std::uint16_t copiedIndex{};
    std::uint32_t copiedSource{};
    const auto row = g_native.manager + kQueueRows + count * kQueueStride;
    if (!read(g_native.manager + kQueueCount, after) || after != count + 1
        || !read(row, copiedSerial) || copiedSerial != serial || !read(row + 0x14, copiedIndex)
        || copiedIndex != index || !read(row + 0x18, copiedGain) || copiedGain != gain
        || !read(row + 0x0C, copiedSource) || copiedSource != source) {
        g_quarantined = true;
        return refused("dispatch_unconfirmed");
    }
    return {Status::delivered, "native_queue_copied"};
}
} // namespace

Result initialize() noexcept {
    const DWORD thread = GetCurrentThreadId();
    DWORD empty = 0;
    if (!g_thread.compare_exchange_strong(empty, thread, std::memory_order_acq_rel)
        && empty != thread)
        return refused("wrong_thread");
    if (g_quarantined) return refused("quarantined");
    if (!g_attempted) {
        g_attempted = true;
        g_resolved = resolve_native();
    }
    return g_resolved ? Result{Status::ready, "targets_resolved"} : refused("signature");
}

bool may_service_current_thread() noexcept {
    const auto thread = g_thread.load(std::memory_order_acquire);
    return thread == 0 || thread == GetCurrentThreadId();
}

Result ready(std::uint8_t ingredientOrdinal) noexcept {
    // SEH boundary contains no objects requiring unwinding. A bad native readiness/classifier
    // call quarantines this optional presentation adapter without touching the wallet.
    __try {
        std::uint16_t index{};
        std::uint64_t count{};
        std::int32_t serial{};
        return preflight(ingredientOrdinal, index, count, serial);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_quarantined = true;
        return refused("readiness_fault");
    }
}

Result present(std::uint8_t ingredientOrdinal, std::int32_t committedGain) noexcept {
    if (committedGain <= 0) return refused("nonpositive_gain");
    __try {
        return deliver(ingredientOrdinal, committedGain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_quarantined = true;
        return refused("dispatch_fault");
    }
}
} // namespace sunrise::client::material_toast
