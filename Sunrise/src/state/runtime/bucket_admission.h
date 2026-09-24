#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../account/account_state.h"
#include "../build_data/inventory/buckets/definition.h"

namespace sunrise::state::runtime::detail {

/** Counts occupied rows and remembers the oldest replaceable row in their owning array. */
struct BucketAdmission {
    std::size_t count{};
    std::size_t oldest{(std::numeric_limits<std::size_t>::max)()};
    std::int32_t serial{(std::numeric_limits<std::int32_t>::max)()};

    /** Equipped rows count toward capacity but must not be offered as eviction candidates. */
    void include(std::size_t index, std::int32_t mutationSerial) noexcept {
        ++count;
        if (oldest == (std::numeric_limits<std::size_t>::max)() || mutationSerial < serial) {
            oldest = index;
            serial = mutationSerial;
        }
    }

    /** Chooses an append or FIFO replacement; a full ordinary bucket refuses the insertion. */
    [[nodiscard]] bool select(const build_data::inventory::buckets::Descriptor& bucket,
                              std::size_t appendIndex,
                              std::size_t& index) const noexcept {
        index = appendIndex;
        if (count < bucket.slotCount) {
            return true;
        }
        if (count != bucket.slotCount
            || (bucket.policyFlags & build_data::inventory::buckets::kFifo) == 0
            || oldest >= appendIndex) {
            return false;
        }
        index = oldest;
        return true;
    }
};

/** Character rows share one FIFO order across instances and stacks. */
inline constexpr std::size_t kCharacterStackAdmissionBase =
    account::inventory::kCharacterItemCapacity;
inline constexpr std::size_t kCharacterAdmissionCapacity =
    kCharacterStackAdmissionBase + account::inventory::kCharacterStackCapacity;

/** Counts equipped rows but chooses eviction candidates only from held instances and stacks. */
[[nodiscard]] bool
character_bucket_admission(const CharacterState& character,
                           const build_data::inventory::buckets::Descriptor& bucket,
                           BucketAdmission& occupancy) noexcept;
/** Removes a selected combined-array row while keeping both saved arrays dense. */
[[nodiscard]] bool erase_character_bucket_row(CharacterState& character, std::size_t row) noexcept;

} // namespace sunrise::state::runtime::detail
