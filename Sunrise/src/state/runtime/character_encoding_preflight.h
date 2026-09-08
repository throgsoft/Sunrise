#pragma once

#include <array>
#include <memory>
#include <new>

#include "../../middleware/datagen/family4/character/character_encoder.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../equipment/light/resolution/configured_equipment_light_resolver.h"

namespace sunrise::state::runtime::detail {

/** Validate the complete outbound character before admitting an inventory grant.
 * Loadout resolution alone omits persisted stacks and collectible prerequisite rows.
 */
inline bool
character_encoding_preflight(const AccountState& candidate,
                             std::size_t characterIndex,
                             const middleware::datagen::family4::loadout::ResolvedLoadout& resolved,
                             bool requireCollectibleSpace = true) noexcept {
    namespace character = middleware::datagen::family4::character;
    if (characterIndex >= candidate.characterCount || characterIndex >= candidate.characters.size())
        return false;
    struct Scratch {
        equipment::light::Evaluation light{};
        std::array<std::byte, character::layout::kObjectSize> bytes{};
    };
    const std::unique_ptr<Scratch> scratch(new (std::nothrow) Scratch{});
    return scratch
           && equipment::light::resolution::resolve(candidate, characterIndex, scratch->light)
           && character::encode(candidate.characters[characterIndex],
                                resolved,
                                scratch->light,
                                scratch->bytes,
                                requireCollectibleSpace);
}

} // namespace sunrise::state::runtime::detail
