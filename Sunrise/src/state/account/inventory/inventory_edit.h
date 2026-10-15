#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::account::inventory {

/** Which saved list one quantity edit names; it must be the one the item's bucket uses. */
enum class EditScope : std::uint8_t { profile, characterItem, characterStack };

/** One row the caller read and the quantity that replaces it. */
struct QuantityEdit {
    EditScope scope{};
    /** Character expected to be selected; a profile edit publishes beside it too. */
    std::uint64_t characterSoid{};
    /** Zero for plain profile rows and character stacks. */
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
    std::int32_t expectedQuantity{};
    /** Zero removes the row without dismantle materials. */
    std::int32_t quantity{};
};

/** Identities one committed edit changed, filled only on success. */
struct QuantityChange {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    /** The row's new serial, or zero when the row was removed. */
    std::int32_t mutationSerial{};
};

/**
 * Replaces one unchanged, unequipped row's quantity in its own savepoint, so an enclosing store
 * transaction can still roll it back. Instanced rows are never removed here: an unequipped item
 * leaves through state::prepare_item_removal, and profile action sources are refused.
 * @param edit Expected row identity, quantity and replacement.
 * @param change Receives the changed identities on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool set_quantity(const QuantityEdit& edit,
                                QuantityChange& change,
                                const char** refusal = nullptr) noexcept;

} // namespace sunrise::state::account::inventory
