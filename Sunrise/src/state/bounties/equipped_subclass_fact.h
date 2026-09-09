#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "gameplay_progress.h"

namespace sunrise::state::gameplay_equipment {
/** Value-owned State observation; no native, equipment or catalog pointers escape. */
struct SubclassFact {
    std::uint64_t accountSoid{}, characterSoid{}, subclassInstanceSoid{};
    std::uint32_t subclassDefinitionHash{};
    std::uint8_t characterClass{0xFF};
};
static_assert(sizeof(SubclassFact) <= 32);
struct SubclassDefinition {
    std::uint32_t hash;
    std::uint8_t characterClass;
    bounties::gameplay::Damage affinity;
};
using bounties::gameplay::Damage;
/** Subclass affinity policy; manifest defaultDamageType is not subclass affinity. */
inline constexpr std::array<SubclassDefinition, 9> kSubclasses{{
    {0x4F91DC97U,1,Damage::arc}, {0xD8B8D1FCU,1,Damage::solar},
    {0xC0483D8BU,1,Damage::voidDamage}, {0xB0554739U,0,Damage::arc},
    {0xB920CE9AU,0,Damage::solar}, {0xC99B33E9U,0,Damage::voidDamage},
    {0x686A154AU,2,Damage::arc}, {0xCF88FEA5U,2,Damage::solar},
    {0xE7BC88B0U,2,Damage::voidDamage},
}};
/** InstalledIdentity validates the definition hash against this installation. */
template<class InstalledIdentity>
[[nodiscard]] std::optional<Damage> affinity_for(const SubclassFact& fact,
    std::uint64_t account, std::uint64_t character, InstalledIdentity&& installed) noexcept {
    if (!account || !character || fact.accountSoid != account || fact.characterSoid != character
        || !fact.subclassInstanceSoid) return {};
    for (const auto& row : kSubclasses)
        if (row.hash == fact.subclassDefinitionHash && row.characterClass == fact.characterClass
            && installed(row.hash)) return row.affinity;
    return {};
}
}
