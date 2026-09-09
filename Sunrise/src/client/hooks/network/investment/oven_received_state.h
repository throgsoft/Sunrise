#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../../middleware/datagen/family4/account/layout.h"

namespace sunrise::client::hooks::network::investment::oven_received {
namespace account = middleware::datagen::family4::account::layout;
namespace inventory = middleware::datagen::family4::inventory::layout;

// Received Family-4 input banks, not the native derived flag bank or server State.
inline constexpr std::size_t kFirstRecipeFlag = 25, kFirstIngredientValue = 204;
inline constexpr std::size_t kTutorialValue = 5627;

struct Inputs {
    std::uint64_t owner{}, selectedCharacter{};
    std::array<std::uint8_t, 22> recipes{};
    std::array<std::int32_t, 20> ingredients{};
    std::int32_t tutorial{};
    std::uint32_t profileCount{};
    // Cookies, Essence and return gifts arrive in this sparse native inventory. Comparing
    // its semantic fields avoids a second fixed item-index table and detects row movement.
    std::array<inventory::Entry, account::kProfileItemCapacity> rows{};
};

inline bool same(const Inputs& a, const Inputs& b) noexcept {
    if (a.owner != b.owner || a.selectedCharacter != b.selectedCharacter || a.recipes != b.recipes
        || a.ingredients != b.ingredients || a.tutorial != b.tutorial
        || a.profileCount != b.profileCount)
        return false;
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        const auto& left = a.rows[i];
        const auto& right = b.rows[i];
        const bool leftLive = left.definitionIndex != 0xFFFF && left.quantity > 0;
        const bool rightLive = right.definitionIndex != 0xFFFF && right.quantity > 0;
        if (leftLive != rightLive) return false;
        if (leftLive
            && (left.definitionIndex != right.definitionIndex
                || left.instanceSoid != right.instanceSoid || left.quantity != right.quantity
                || left.mutationSerial != right.mutationSerial || left.flags != right.flags))
            return false;
    }
    return true;
}

// Output is caller-owned scratch, unusable on failure. Keep it off the game's stack.
template <class Read>
[[nodiscard]] bool read_inputs(std::uintptr_t address, Read&& read, Inputs& output) noexcept {
    if (!address || address > (std::numeric_limits<std::uintptr_t>::max)() - account::kObjectSize)
        return false;
    const auto field = [&](std::size_t offset, auto& value) {
        return read(address + offset, std::as_writable_bytes(std::span(&value, 1)));
    };
    if (!field(account::kAccountSoidOffset, output.owner) || !output.owner
        || !field(account::kSelectedCharacterSoidOffset, output.selectedCharacter)
        || !field(account::kAcquiredFlagsOffset + kFirstRecipeFlag, output.recipes)
        || !field(account::kObjectiveValuesOffset + kFirstIngredientValue * sizeof(std::int32_t),
                  output.ingredients)
        || !field(account::kObjectiveValuesOffset + kTutorialValue * sizeof(std::int32_t),
                  output.tutorial)
        || !field(account::kProfileItemCountOffset, output.profileCount)
        || output.profileCount > output.rows.size()
        || !read(address + account::kProfileItemsOffset,
                 std::as_writable_bytes(std::span(output.rows))))
        return false;
    std::uint64_t ownerAfter{}, selectedAfter{};
    std::uint32_t countAfter{};
    return field(account::kAccountSoidOffset, ownerAfter) && ownerAfter == output.owner
           && field(account::kSelectedCharacterSoidOffset, selectedAfter)
           && selectedAfter == output.selectedCharacter
           && field(account::kProfileItemCountOffset, countAfter)
           && countAfter == output.profileCount;
}

// Publication alone is not a receipt. Only a complete native read advances this tracker.
struct Tracker {
    Inputs previous{};
    bool initialized{};
    [[nodiscard]] bool observe(const Inputs& received) noexcept {
        if (initialized && same(received, previous)) return false;
        previous = received;
        initialized = true;
        return true;
    }
};
} // namespace sunrise::client::hooks::network::investment::oven_received
