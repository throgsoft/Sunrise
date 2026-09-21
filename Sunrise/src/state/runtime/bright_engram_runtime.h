#pragma once

#include <cstdint>

#include "../build_data/eververse/manifest_catalog.h"
#include "eververse_runtime.h"

namespace sunrise::state {
struct AccountState;
struct PendingRecordRewardGrant;
} // namespace sunrise::state

namespace sunrise::state::bright_engrams {
enum class GrantSourceKind : std::uint8_t { unrelated, held, preview, unsupportedVariant };
/** Installed native identity plus the local manual silver-sack catalog; never a name/hash list.
 * Preview dummies and variants of a supported preview that cannot be held are non-grantable.
 */
[[nodiscard]] GrantSourceKind classify_grant_source(std::uint16_t itemIndex,
                                                    std::uint32_t expectedHash) noexcept;

struct RedemptionContext {
    std::uint64_t sourceInstanceSoid{};
    std::uint32_t sourceDefinitionHash{};
    build_data::eververse::EngramSelection selection{};
    eververse::NativeOwnership ownership{};
    bool operator==(const RedemptionContext&) const = default;
};

/** Validates the held source before resolving its preview catalog and fixed Sunrise draw. */
[[nodiscard]] bool prepare_redemption(std::uint64_t sourceSoid,
                                      std::int16_t requestedDefinitionIndex,
                                      PendingRecordRewardGrant& mutation) noexcept;
[[nodiscard]] bool materialize_redemption(const AccountState& current,
                                          const PendingRecordRewardGrant& mutation,
                                          AccountState& after) noexcept;
/** Called inside the reward publication path, after encoding/staging has succeeded. */
[[nodiscard]] bool commit_redemption(PendingRecordRewardGrant& mutation) noexcept;
} // namespace sunrise::state::bright_engrams
