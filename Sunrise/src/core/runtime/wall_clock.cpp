#include "wall_clock.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>

namespace sunrise::core::runtime {
namespace {
std::mutex clockMutex;
std::int64_t issuedClock{};
std::chrono::steady_clock::time_point issuedAt{};

std::int64_t current_locked() noexcept {
    const auto wall = server_clock_seconds();
    if (!issuedClock) return wall;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - issuedAt)
                             .count();
    return (std::max)(wall, issuedClock + elapsed);
}
} // namespace
std::int64_t investment_clock_seconds() noexcept {
    const std::lock_guard lock(clockMutex);
    return current_locked();
}
std::uint64_t next_family5_clock_seconds() noexcept {
    const std::lock_guard lock(clockMutex);
    issuedClock = (std::max)(current_locked(), issuedClock + 1);
    issuedAt = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(issuedClock);
}
bool investment_deadline(std::int64_t lifetimeSeconds, std::int32_t& deadline) noexcept {
    deadline = 0;
    const auto now = investment_clock_seconds();
    const auto maximum = (std::numeric_limits<std::int32_t>::max)();
    if (now <= 0 || now >= maximum || lifetimeSeconds <= 0 || lifetimeSeconds > maximum - now)
        return false;
    deadline = static_cast<std::int32_t>(now + lifetimeSeconds);
    return true;
}
} // namespace sunrise::core::runtime
