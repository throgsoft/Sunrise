#include "bucket_admission.h"

#include <algorithm>

#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail {
namespace inventory = account::inventory;
namespace buckets = build_data::inventory::buckets;

/** Placement overrides the authored bucket only for Lost Items. */
[[nodiscard]] static bool
in_bucket(const inventory::Item& item, std::uint8_t bucketId, bool& matches) noexcept {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)) {
        return false;
    }
    const auto placedBucket = item.placement == inventory::ItemPlacement::postmaster
                                  ? buckets::kPostmasterBucketId
                                  : definition.bucketId;
    matches = placedBucket == bucketId;
    return true;
}

bool character_bucket_admission(const CharacterState& character,
                                const buckets::Descriptor& bucket,
                                BucketAdmission& occupancy) noexcept {
    occupancy = {};
    if (character.inventory.count > character.inventory.values.size()
        || character.stacks.count > character.stacks.values.size()) {
        return false;
    }
    for (const auto& equipped : character.equipment.slots) {
        bool matches = false;
        if (equipped && !in_bucket(*equipped, bucket.bucketId, matches)) {
            return false;
        }
        occupancy.count += matches;
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const auto& item = character.inventory.values[index];
        bool matches = false;
        if (!in_bucket(item, bucket.bucketId, matches)) {
            return false;
        }
        if (matches) {
            occupancy.include(index, item.mutationSerial);
        }
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& item = character.stacks.values[index];
        build_data::items::Definition definition{};
        if (!build_data::find_item_definition_hash(item.definitionHash, definition)) {
            return false;
        }
        if (definition.bucketId == bucket.bucketId) {
            occupancy.include(kCharacterStackAdmissionBase + index, item.mutationSerial);
        }
    }
    return true;
}

bool erase_character_bucket_row(CharacterState& character, std::size_t row) noexcept {
    if (row < kCharacterStackAdmissionBase) {
        auto& held = character.inventory;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    } else {
        auto& held = character.stacks;
        row -= kCharacterStackAdmissionBase;
        if (row >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(row) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(row));
        held.values[--held.count] = {};
    }
    return true;
}
} // namespace sunrise::state::runtime::detail
