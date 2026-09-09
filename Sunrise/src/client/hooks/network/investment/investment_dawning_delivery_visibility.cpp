#include "investment_dawning_delivery_visibility.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../content/handles/handle_resolver.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game/content.h"
#include "investment_derived_rebuild.h"

namespace sunrise::client::hooks::network::investment {
namespace {

// Retained build 81964380664e7fce, deliberately Zavala-only. Vendor16/tag8131908E,
// interaction21/category1/sale1/offer290 costs cookie289. Native expression at blob
// 1D510 is FLAG779 FLAG828 AND; sale1's +120 wrapper is FLAG779 alone.
// FLAG779's producer is unproved. Bypass this retained event gate for branch testing;
// cookie289 grants computed828, and ownership828 remains native. No global flag is changed.
// Native evaluator554310: opcode11 pushes its u32 operand; boolean operators use
// zero/nonzero. Thus literal1 preserves the cookie predicate without inventing ownership.
constexpr std::uint32_t kTableTag = 0x8131908EU;
constexpr std::uint32_t kVendorIndexTag = 0x8131931DU, kItemIndexTag = 0x81327CCBU;
constexpr std::size_t kVendorBytes = 122664;
struct Instruction {
    std::uint32_t opcode, operand;
    friend bool operator==(const Instruction&, const Instruction&) = default;
};
constexpr Instruction kOriginal{1, 779}, kReplacement{11, 1};
constexpr std::array<Instruction, 3> kExpression{{kOriginal, {1, 828}, {4, 0xFFFFFFFFU}}};
struct Descriptor {
    std::uint64_t count;
    std::int64_t relative;
};
static_assert(sizeof(Descriptor) == 16 && sizeof(Instruction) == 8);
struct Patch {
    std::uintptr_t address{};
    DWORD originalProtection{};
    bool protectionPending{};
};
std::array<Patch, 2> g_patches{};
std::uintptr_t g_vendorIndex{}, g_itemIndex{};
std::size_t g_count{};
std::uintptr_t g_table{};
const char* g_prepareStage = "idle";
const char* g_reportedFailure{};
SRWLOCK g_lock = SRWLOCK_INIT;

template <class T> bool read(std::uintptr_t address, T& value) noexcept {
    return memory::read_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&value, 1)));
}

/** Resolves a signed self-relative pointer without overflowing either address endpoint. */
bool relative_at(std::uintptr_t at, std::int64_t offset, std::uintptr_t& result) noexcept {
    constexpr auto limit = static_cast<std::uintptr_t>((std::numeric_limits<std::int64_t>::max)());
    if (at > limit) return false;
    const auto base = static_cast<std::int64_t>(at);
    if (offset < -base || offset > static_cast<std::int64_t>(limit) - base) return false;
    result = static_cast<std::uintptr_t>(base + offset);
    return result >= 4 && result <= limit - 32;
}

/** Validates the retained typed array, including its duplicated count and readable extent. */
bool array_at(std::uintptr_t at,
              std::uint32_t type,
              std::size_t stride,
              std::uint64_t maximum,
              Descriptor& desc,
              std::uintptr_t& data) noexcept {
    std::uintptr_t header{};
    std::uint64_t count{};
    std::uint32_t marker{}, actualType{};
    if (!read(at, desc) || desc.count == 0 || desc.count > maximum || !relative_at(at, 8, header)
        || !relative_at(header, desc.relative, header) || !read(header - 4, marker)
        || marker != 0x80809FBDU || !read(header, count) || count != desc.count
        || !read(header + 8, actualType) || actualType != type)
        return false;
    data = header + 16;
    if (desc.count > ((std::numeric_limits<std::uintptr_t>::max)() - data) / stride) return false;
    std::byte last{};
    return read(data + static_cast<std::size_t>(desc.count) * stride - 1, last);
}

bool resolve_tag(std::uint32_t tag, std::uintptr_t& table) noexcept {
    content::handles::Source source{};
    source.tablesSlot =
        reinterpret_cast<std::uintptr_t>(targets::game::content::get().contentHandleTablesSlot);
    source.read = &memory::read_current_process;
    return content::handles::resolve(source, tag, table);
}

bool resolve_table(std::uintptr_t& table) noexcept {
    return resolve_tag(kTableTag, table);
}

bool inside(std::uintptr_t table, std::uintptr_t at, std::size_t size) noexcept {
    return at >= table && at - table <= kVendorBytes && size <= kVendorBytes - (at - table);
}

/** Checks every retained identity before borrowing one instruction. No package file edits. */
bool prepare(std::array<Patch, 2>& staged,
             std::uintptr_t& table,
             std::uintptr_t& vendorIndex,
             std::uintptr_t& itemIndex) noexcept {
    Descriptor desc{};
    std::uintptr_t data{}, rows{}, sales{};
    std::uint64_t size{};
    std::uint32_t hash{}, tag{};
    g_prepareStage = "vendor_residency";
    if (!resolve_table(table)) return false;
    g_prepareStage = "index_residency";
    if (!resolve_tag(kVendorIndexTag, vendorIndex) || !resolve_tag(kItemIndexTag, itemIndex))
        return false;
    g_prepareStage = "vendor_index";
    if (!array_at(vendorIndex + 8, 0x8080784EU, 24, 511, desc, data) || desc.count != 511
        || !read(data + 16 * 24, hash) || hash != 69482069U || !read(data + 16 * 24 + 16, tag)
        || tag != kTableTag)
        return false;
    g_prepareStage = "item_index";
    if (!array_at(itemIndex + 8, 0x80807BE8U, 24, 15424, desc, data) || desc.count != 15424
        || !read(data + 289 * 24, hash) || hash != 0xA1354D2BU || !read(data + 290 * 24, hash)
        || hash != 0xBA79053FU)
        return false;
    g_prepareStage = "vendor_layout";
    if (!read(table, size) || size != kVendorBytes
        || !array_at(table + 80, 0x80807857U, 80, 42, desc, rows) || desc.count != 42
        || !inside(table, rows, 42 * 80)
        || !array_at(table + 48, 0x80807861U, 184, 290, desc, sales) || desc.count != 290
        || !inside(table, sales, 290 * 184))
        return false;
    const auto screen = rows + 21 * 80;
    std::array<std::uint32_t, 2> identity{};
    std::array<std::uint32_t, 6> suffix{};
    std::array<std::uint64_t, 2> empty{};
    g_prepareStage = "interaction_identity";
    if (!read(screen, identity) || identity != std::array<std::uint32_t, 2>{0xFFFF, 2716011929U}
        || !read(screen + 24, empty) || empty != std::array<std::uint64_t, 2>{}
        || !read(screen + 56, suffix) || suffix != std::array<std::uint32_t, 6>{1, 0, 0, 0, 0, 0}
        || !array_at(screen + 40, 0x8080785BU, 24, 1, desc, data) || desc.count != 1
        || !inside(table, data, 24))
        return false;
    // The exact category's sale is the gift offer; its price is one physical cookie.
    std::uint16_t item{};
    std::int32_t category{};
    std::array<std::uint32_t, 2> cost{};
    g_prepareStage = "offer_identity";
    if (!read(sales + 184 + 70, item) || item != 290 || !read(sales + 184 + 100, category)
        || category != 1 || !array_at(sales + 184 + 32, 0x80807865U, 8, 1, desc, data)
        || desc.count != 1 || !inside(table, data, 8) || !read(data, cost)
        || cost != std::array<std::uint32_t, 2>{289, 1})
        return false;
    std::array<Instruction, 3> expression{};
    g_prepareStage = "interaction_expression";
    if (!array_at(screen + 8, 0x80807D31U, 8, 3, desc, data) || desc.count != 3
        || !inside(table, data, sizeof expression) || !read(data, expression)
        || expression != kExpression || data % alignof(Instruction) != 0)
        return false;
    staged[0].address = data;
    // The same exact offer's sale +120 condition is a one-expression wrapper containing
    // FLAG779 alone (blob D790). It must agree with the dialog's branch-testing bypass.
    std::uintptr_t wrapper{}, saleExpression{};
    Instruction saleGate{};
    g_prepareStage = "sale_expression";
    if (!array_at(sales + 184 + 120, 0x80807D2FU, 16, 1, desc, wrapper) || desc.count != 1
        || !inside(table, wrapper, 16)
        || !array_at(wrapper, 0x80807D31U, 8, 1, desc, saleExpression) || desc.count != 1
        || !inside(table, saleExpression, 8) || !read(saleExpression, saleGate)
        || saleGate != kOriginal || saleExpression % alignof(Instruction) != 0
        || saleExpression == staged[0].address)
        return false;
    staged[1].address = saleExpression;
    // Refuse aliased expression storage: changing another dialog/sale is outside this repair.
    std::array<unsigned, 2> owners{};
    g_prepareStage = "expression_ownership";
    const auto inspect = [&](std::uintptr_t at) {
        Descriptor expressionDesc{};
        std::uintptr_t expressionData{};
        if (!read(at, expressionDesc)) return false;
        if (expressionDesc.count == 0 && expressionDesc.relative == 0) return true;
        if (!array_at(at, 0x80807D31U, 8, 512, expressionDesc, expressionData)
            || !inside(table, expressionData, static_cast<std::size_t>(expressionDesc.count) * 8))
            return false;
        for (std::size_t i = 0; i < staged.size(); ++i)
            if (expressionData < staged[i].address + sizeof(Instruction)
                && staged[i].address < expressionData + expressionDesc.count * 8)
                ++owners[i];
        return true;
    };
    for (std::size_t i = 0; i < 42; ++i)
        if (!inspect(rows + i * 80 + 8)) return false;
    for (std::size_t i = 0; i < 290; ++i) {
        for (auto offset : {8U, 120U, 160U}) {
            Descriptor wrappers{};
            std::uintptr_t wrapperData{};
            const auto at = sales + i * 184 + offset;
            if (!read(at, wrappers)) return false;
            if (wrappers.count == 0 && wrappers.relative == 0) continue;
            if (!array_at(at, 0x80807D2FU, 16, 512, wrappers, wrapperData)
                || !inside(table, wrapperData, static_cast<std::size_t>(wrappers.count) * 16))
                return false;
            for (std::size_t j = 0; j < wrappers.count; ++j)
                if (!inspect(wrapperData + j * 16)) return false;
        }
    }
    return owners[0] == 1 && owners[1] == 1;
}

bool restore_protection(Patch& patch) noexcept {
    if (!patch.protectionPending) return true;
    DWORD ignored{};
    if (!VirtualProtect(reinterpret_cast<void*>(patch.address),
                        sizeof(Instruction),
                        patch.originalProtection,
                        &ignored))
        return false;
    patch.protectionPending = false;
    return true;
}

/** Compare before writing, restore original protection, then verify the exact instruction result.
 */
bool write(Patch& patch, bool restore) noexcept {
    const Instruction expected = restore ? kReplacement : kOriginal;
    const Instruction desired = restore ? kOriginal : kReplacement;
    Instruction current{};
    if (!read(patch.address, current) || current != expected) return false;
    auto* destination = reinterpret_cast<void*>(patch.address);
    if (!patch.protectionPending) {
        if (!VirtualProtect(destination, sizeof desired, PAGE_READWRITE, &patch.originalProtection))
            return false;
        patch.protectionPending = true;
    }
    SIZE_T written{};
    const bool copied =
        WriteProcessMemory(GetCurrentProcess(), destination, &desired, sizeof desired, &written)
        && written == sizeof desired;
    const bool protectedAgain = restore_protection(patch);
    return copied && protectedAgain && read(patch.address, current) && current == desired;
}

bool rollback() noexcept {
    if (g_count == 0) return true;
    std::uintptr_t table{};
    // Never write a saved address after its tag has been unloaded/replaced.
    std::uintptr_t vendorIndex{}, itemIndex{};
    if (!resolve_table(table) || table != g_table || !resolve_tag(kVendorIndexTag, vendorIndex)
        || vendorIndex != g_vendorIndex || !resolve_tag(kItemIndexTag, itemIndex)
        || itemIndex != g_itemIndex)
        return false;
    bool ok = true;
    for (std::size_t i = g_count; i > 0; --i) {
        auto& patch = g_patches[i - 1];
        Instruction current{};
        if (!read(patch.address, current)) {
            ok = false;
            continue;
        }
        if (current == kOriginal) {
            if (!restore_protection(patch)) ok = false;
        } else if (!write(patch, true)) {
            // A third-party value is never overwritten, but still release our page protection.
            (void)restore_protection(patch);
            ok = false;
        }
    }
    if (ok) {
        g_count = 0;
        g_table = g_vendorIndex = g_itemIndex = 0;
    }
    return ok;
}
} // namespace

bool is_dawning_delivery_vendor(const void* vendor) noexcept {
    std::uintptr_t table{};
    std::uint64_t size{};
    return vendor != nullptr && resolve_table(table)
           && table == reinterpret_cast<std::uintptr_t>(vendor) && read(table, size)
           && size == kVendorBytes;
}

void apply_dawning_delivery_visibility() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_count != 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    std::array<Patch, 2> staged{};
    std::uintptr_t table{};
    std::uintptr_t vendorIndex{}, itemIndex{};
    bool ok = prepare(staged, table, vendorIndex, itemIndex);
    if (ok) {
        g_table = table;
        g_vendorIndex = vendorIndex;
        g_itemIndex = itemIndex;
        for (const auto& patch : staged) {
            g_prepareStage = g_count == 0 ? "interaction_write" : "sale_write";
            g_patches[g_count++] = patch;
            if (!write(g_patches[g_count - 1], false)) {
                ok = false;
                break;
            }
        }
    }
    const bool restored = ok || rollback();
    if (ok) {
        // The picker can admit the newly enabled interaction while derived investment still
        // reflects the old sale expression. Invalidate through the existing freshness gate;
        // never re-enter the native rebuild from inside the picker or repeat this each frame.
        notify_investment_definition_change();
        g_reportedFailure = nullptr;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=dawning_delivery_visibility result=applied vendor=16 interaction=21 "
                         "cookie_flag=828 rebuild_armed=1");
    } else if (!restored || g_reportedFailure != g_prepareStage) {
        // A boot-time content boundary is not proof the destination vendor is resident.
        // The picker can retry later. Report each changed refusal stage rather than hiding
        // the useful distinction between absent content and a rejected expression shape.
        g_reportedFailure = g_prepareStage;
        std::array<char, 240> line{};
        std::snprintf(line.data(),
                      line.size(),
                      "ev=dawning_delivery_visibility result=%s originals_retained=%u stage=%s",
                      restored ? "refused" : "rollback_failed",
                      unsigned(restored),
                      g_prepareStage);
        core::log::write(core::log::Channel::client, core::log::Level::warn, line.data());
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void restore_dawning_delivery_visibility() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!rollback())
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=dawning_delivery_visibility result=restore_failed");
    g_reportedFailure = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}
} // namespace sunrise::client::hooks::network::investment
