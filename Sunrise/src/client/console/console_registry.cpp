#include "console_registry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "../../core/logging/log.h"

namespace sunrise::client::console {
namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Entry, kEntryCapacity> g_entries{};
std::size_t g_count{};

/** @return True when the entry names a command that can actually be run. */
[[nodiscard]] bool well_formed(const Entry& entry) noexcept {
    if (entry.name == nullptr || entry.help == nullptr || entry.handler == nullptr) {
        return false;
    }
    const std::string_view name(entry.name);
    if (name.empty() || name.size() >= kLineCapacity) {
        return false;
    }
    // A required parameter after an optional one could never be supplied, so the table is wrong.
    bool optionalSeen = false;
    for (const Parameter& parameter : entry.parameters) {
        if (parameter.name == nullptr) {
            break;
        }
        if (parameter.optional) {
            optionalSeen = true;
        } else if (optionalSeen) {
            return false;
        }
    }
    return true;
}

/** @return Position of name in the sorted table, or g_count when it is absent. */
[[nodiscard]] std::size_t position_of(std::string_view name) noexcept {
    for (std::size_t index = 0; index < g_count; ++index) {
        if (std::string_view(g_entries[index].name) == name) {
            return index;
        }
    }
    return g_count;
}

} // namespace

bool add(const Entry& entry) noexcept {
    if (!well_formed(entry)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const std::string_view name(entry.name);
    const std::size_t existing = position_of(name);
    if (existing != g_count) {
        // Replacing is kept, because a module may re-register its own table. Doing it in silence is
        // not: two modules claiming one name leaves whichever registered last, and a command that
        // answers for a different parameter list than the one being read about is a fault that
        // costs a session to work out.
        std::array<char, kLineCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=console stage=register result=replaced name=%s",
                                        entry.name);
        if (count > 0) {
            core::log::write(
                core::log::Channel::client,
                core::log::Level::warn,
                {line.data(), (std::min)(static_cast<std::size_t>(count), line.size() - 1)});
        }
        g_entries[existing] = entry;
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (g_count == g_entries.size()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    // Kept sorted on insert, so completion and `help` both walk it in the order they print.
    std::size_t at = g_count;
    while (at != 0 && std::string_view(g_entries[at - 1].name) > name) {
        g_entries[at] = g_entries[at - 1];
        --at;
    }
    g_entries[at] = entry;
    ++g_count;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_entries = {};
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

std::size_t count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t value = g_count;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

bool find(std::string_view name, Entry& entry) noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t at = position_of(name);
    const bool found = at != g_count;
    if (found) {
        entry = g_entries[at];
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool snapshot(std::span<Entry> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_lock);
    const bool fits = output.size() >= g_count;
    if (fits) {
        std::copy_n(g_entries.begin(), g_count, output.begin());
        count = g_count;
    }
    ReleaseSRWLockShared(&g_lock);
    return fits;
}

} // namespace sunrise::client::console
