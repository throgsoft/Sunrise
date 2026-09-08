#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::account::inventory::dawning {

inline constexpr std::uint32_t kOvenHash = 2742763040U;
inline constexpr std::uint32_t kEssenceHash = 1878753218U;
inline constexpr std::uint32_t kCombineHash = 2161676424U;
inline constexpr std::uint32_t kMasterworkHash = 2525467079U;
inline constexpr std::uint32_t kBurntCookieHash = 2281231178U;
inline constexpr std::size_t kIngredientCount = 20;
inline constexpr std::size_t kRecipeCount = 22;
// Retained build's material value and acquired-recipe banks, not inventory bucket rows.
inline constexpr std::uint16_t kFirstIngredientValue = 204;
inline constexpr std::uint16_t kFirstRecipeFlag = 25;

struct State {
    std::array<std::int32_t, kIngredientCount> ingredients{};
    std::array<std::uint8_t, kRecipeCount> recipes{};
    bool initialized{};
    bool operator==(const State&) const = default;
};

/** Server exchange identity carried only while one delivery is prepared. */
struct DeliveryContext {
    std::int64_t stagedAt{};
    std::uint32_t vendorHash{}, offerHash{};
    bool operator==(const DeliveryContext&) const = default;
};

/** Semantic identities only. Prices, stacks, socket compatibility and vendor rows are
 * resolved from the upstream installed catalogs before any exchange is authorized. */
struct RecipeIdentity {
    std::uint32_t plugHash, masterworkedPlugHash, cookieHash;
};
inline constexpr std::array<RecipeIdentity, kRecipeCount> kRecipes{{
    {2546992977U, 3884817923U, 2704624939U}, {3457862632U, 1289611082U, 2704624938U},
    {2567160079U, 3766828181U, 2704624937U}, {1566816806U, 1455708620U, 2704624936U},
    {2680500173U, 3593525111U, 2704624943U}, {2690793028U, 1776437198U, 2704624942U},
    {3468840443U, 2252566553U, 2704624941U}, {1972037794U, 1736316176U, 2704624940U},
    {1556827161U, 4164579835U, 2704624931U}, {1040576784U, 2667777186U, 2704624930U},
    {1883538310U, 3051102764U, 4041767214U}, {3626109295U, 1067152117U, 4041767215U},
    {2412808648U, 1481653034U, 4041767212U}, {1460362289U, 3334529507U, 4041767213U},
    {461052802U, 2164922480U, 4041767210U},  {3827035483U, 1319397369U, 4041767211U},
    {3986202916U, 4032646574U, 4041767208U}, {423273389U, 4022027991U, 4041767209U},
    {2800565166U, 923317028U, 4041767206U},  {2789946583U, 1655354797U, 4041767207U},
    {1551725739U, 3407940349U, 4024989627U}, {4165341394U, 1494246964U, 4024989626U},
}};
struct IngredientIdentity {
    std::uint32_t pickupHash, plugHash;
};
inline constexpr std::array<IngredientIdentity, kIngredientCount> kIngredients{{
    {4057968891U, 3010348881U}, {4057968890U, 3010348880U}, {4057968889U, 3010348883U},
    {4057968888U, 3010348882U}, {4057968895U, 3010348885U}, {4057968894U, 3010348884U},
    {4057968893U, 3010348887U}, {4057968892U, 3010348886U}, {4057968883U, 3010348889U},
    {4057968882U, 3010348888U}, {3173226942U, 3331998400U}, {3173226943U, 3331998401U},
    {3173226940U, 3331998402U}, {3173226941U, 3331998403U}, {3173226938U, 3331998404U},
    {3173226939U, 3331998405U}, {3173226936U, 3331998406U}, {3173226937U, 3331998407U},
    {3173226934U, 3331998408U}, {3173226935U, 3331998409U},
}};

[[nodiscard]] constexpr std::size_t ingredient(std::uint32_t hash) noexcept {
    for (std::size_t i = 0; i < kIngredients.size(); ++i) {
        if (hash == kIngredients[i].pickupHash || hash == kIngredients[i].plugHash) return i;
    }
    return kIngredients.size();
}
[[nodiscard]] constexpr bool cookie(std::uint32_t hash) noexcept {
    if (hash == kBurntCookieHash) return true;
    for (const auto& recipe : kRecipes)
        if (hash == recipe.cookieHash) return true;
    return false;
}

} // namespace sunrise::state::account::inventory::dawning
