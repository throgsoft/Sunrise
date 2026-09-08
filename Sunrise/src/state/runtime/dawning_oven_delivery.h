#pragma once
#include <array>
#include <cstdint>

#include "runtime.h"

namespace sunrise::state::runtime::detail::dawning {
/** Semantic recipient/offer/cookie joins. Runtime indices come from installed definitions. */
struct DeliveryIdentity {
    std::uint32_t vendorHash, offerHash, cookieHash;
};
inline constexpr std::array<DeliveryIdentity, 23> kDeliveries{{
    {3982706173U, 1232226503U, 2704624931U}, {396892126U, 1792090952U, 2704624942U},
    {3347378076U, 852572659U, 2704624943U},  {1062861569U, 2668197329U, 2704624941U},
    {1265988377U, 3767463780U, 4024989626U}, {1576276905U, 651848582U, 2704624940U},
    {69482069U, 3128493375U, 2704624939U},   {1976548992U, 4228243412U, 2704624938U},
    {3603221665U, 4285179292U, 2704624930U}, {672118013U, 511686498U, 2704624936U},
    {2190858386U, 1679336404U, 4041767210U}, {460529231U, 105811981U, 2704624937U},
    {2398407866U, 579586367U, 4041767211U},  {1735426333U, 3538151240U, 4041767214U},
    {1841717884U, 119290321U, 4041767213U},  {1454616762U, 119290321U, 4041767213U},
    {863940356U, 2397908870U, 4041767212U},  {248695599U, 2598632947U, 4041767215U},
    {2917531897U, 1377534523U, 4041767207U}, {1944634565U, 2591214864U, 4041767206U},
    {880202832U, 3767463780U, 4024989626U},  {1616085565U, 2344716559U, 4024989627U},
    {765357505U, 1851872269U, 4041767209U},
}};

/** Sunrise policy: one uniformly selected installed world item and up to 100 Glimmer,
 * for one cookie. Retains dacdee4's working policy, which explicitly deferred bonus Gifts
 * in Return and seasonal weapons. Retail rarity odds and gift probabilities are not claimed. */
[[nodiscard]] bool stage_delivery(const AccountState& account,
                                  std::uint32_t vendorHash,
                                  std::uint32_t offerHash,
                                  std::int64_t now,
                                  PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] bool materialize_delivery(const AccountState& current,
                                        const PendingRecordRewardGrant& mutation,
                                        AccountState& after) noexcept;
} // namespace sunrise::state::runtime::detail::dawning
