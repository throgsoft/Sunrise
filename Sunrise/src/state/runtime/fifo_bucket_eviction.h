#pragma once

#include <cstddef>

#include "../account/account_state.h"
#include "../build_data/runtime.h"

namespace sunrise::state::runtime::detail {

/**
 * Drops the oldest rows a full first-in-first-out bucket has to evict to admit an arrival.
 * The installed policy names the bucket kFifo, so a full one makes room rather than refusing the
 * item, and kNoTransferOnEviction says the row it drops is discarded instead of moved elsewhere.
 * Only stack rows are evicted: the bucket's other residents are instanced and are not this
 * bucket's to discard.
 * @param character Character whose stack rows are considered.
 * @param bucket Installed descriptor of the bucket being admitted into.
 * @param needed Rows the caller still needs free.
 * @return Rows removed, which is at most needed.
 */
[[nodiscard]] inline std::size_t
evict_oldest_stacks(CharacterState& character,
                    const build_data::inventory::buckets::Descriptor& bucket,
                    std::size_t needed) noexcept {
    namespace buckets = build_data::inventory::buckets;
    if (needed == 0 || (bucket.policyFlags & buckets::kFifo) == 0) {
        return 0;
    }
    std::size_t removed = 0;
    while (removed < needed) {
        std::size_t oldest = character.stacks.count;
        for (std::size_t index = 0; index < character.stacks.count; ++index) {
            build_data::items::Definition held{};
            if (!build_data::find_item_definition_hash(
                    character.stacks.values[index].definitionHash, held)
                || held.bucketId != bucket.bucketId) {
                continue;
            }
            if (oldest == character.stacks.count
                || character.stacks.values[index].mutationSerial
                       < character.stacks.values[oldest].mutationSerial) {
                oldest = index;
            }
        }
        if (oldest == character.stacks.count) {
            break;
        }
        for (std::size_t index = oldest + 1; index < character.stacks.count; ++index) {
            character.stacks.values[index - 1] = character.stacks.values[index];
        }
        character.stacks.values[--character.stacks.count] = {};
        ++removed;
    }
    return removed;
}

} // namespace sunrise::state::runtime::detail
