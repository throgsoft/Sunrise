#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "eververse_receipts.h"

namespace sunrise::state {
struct AccountState;
struct PendingRecordRewardGrant;
} // namespace sunrise::state

namespace sunrise::state::eververse {

/** Semantic wallet identities, resolved and checked against the installed package. */
inline constexpr std::uint32_t kSilverHash = 3147280338U;
inline constexpr std::uint32_t kBrightDustHash = 2817410917U;
inline constexpr std::uint32_t kStoreVendorHash = 3361454721U;

struct Result {
    bool accepted{};
    std::int32_t balance{};
    const char* reason{};
};

/** No real payment service is involved. The installed wallet stack cap bounds this setter. */
[[nodiscard]] Result set_silver(std::int64_t amount) noexcept;
[[nodiscard]] bool read_silver(std::int32_t& amount) noexcept;

/** Local wallet reconciliation, separate from purchases. A successful request advances the
 * durable generation used by the client's account-delta observer to finish its sync wait.
 * Composes with the caller's investment transaction; never grants or charges currency. */
[[nodiscard]] bool synchronize_wallet(std::uint32_t& revision) noexcept;
[[nodiscard]] bool read_wallet_sync(std::uint64_t accountSoid, std::uint32_t& revision) noexcept;

/** Durable acquisition receipts, NOT native Unlock flags or socket permissions. */
[[nodiscard]] bool
read_ownership(std::uint64_t accountSoid, std::uint32_t definitionHash, bool& owned) noexcept;
/** Composes with the caller's investment transaction; use only together with its item grant. */
[[nodiscard]] bool record_ownership(std::uint64_t accountSoid,
                                    std::uint32_t definitionHash) noexcept;

/** Native mapped flag before/after image; projected before commit, persisted with the grant. */
struct NativeOwnership {
    std::uint16_t itemIndex{0xFFFFU};
    std::uint16_t unlockSlot{0xFFFFU};
    std::uint16_t accountFlag{0xFFFFU};
    std::uint8_t before{};
    std::uint8_t after{};
    bool prepared{};
    bool operator==(const NativeOwnership&) const = default;
};

/** activate=true applies the native acquisition flag; false captures its current value. */
[[nodiscard]] bool prepare_native_ownership(std::uint16_t itemIndex,
                                            bool activate,
                                            NativeOwnership& mutation) noexcept;
/** Caller owns the encompassing currency/source/grant transaction. No standalone grant. */
[[nodiscard]] bool write_native_ownership(const NativeOwnership& mutation) noexcept;

/** Single owned profile copy consumed by the authored opcode-402 Unlock action. */
struct UnlockContext {
    NativeOwnership ownership{};
    std::uint64_t sourceInstanceSoid{};
    std::uint32_t itemHash{};
    std::uint16_t itemIndex{0xFFFFU};
    std::int8_t bucketSelector{-1};
    bool requestHasInstance{};
    bool operator==(const UnlockContext&) const = default;
};

[[nodiscard]] bool supports_manual_unlock(std::uint16_t itemIndex) noexcept;
[[nodiscard]] bool prepare_unlock(bool hasInstance,
                                   std::uint64_t instanceSoid,
                                   std::uint16_t itemIndex,
                                   std::int32_t expectedQuantity,
                                   std::int8_t bucketSelector,
                                   PendingRecordRewardGrant& mutation,
                                   const char*& reason) noexcept;
[[nodiscard]] bool materialize_unlock(const AccountState& current,
                                       const PendingRecordRewardGrant& mutation,
                                       AccountState& after) noexcept;
[[nodiscard]] bool commit_unlock(const PendingRecordRewardGrant& mutation) noexcept;

/** Exact sale identity and receipt generation bound to one prepared currency/grant transition. */
struct PurchaseContext {
    std::optional<PurchaseReceipt> receipt{};
    NativeOwnership ownership{};
    /** The supported pack has two owned cosmetics in addition to its source acquisition. */
    std::array<NativeOwnership, 2> extraOwnership{};
    std::size_t extraOwnershipCount{};
    std::uint32_t vendorHash{};
    std::uint32_t itemHash{};
    std::uint32_t currencyHash{};
    std::uint32_t cost{};
    std::int32_t quantity{};
    std::int32_t purchasesBefore{};
    std::uint16_t vendorIndex{};
    std::uint16_t saleIndex{};
    /** Authored secondary wrapper, or 0xFFFF for an ordinary direct delivery. */
    std::uint16_t secondaryWrapperIndex{0xFFFFU};
    bool recordOwnership{};
    bool operator==(const PurchaseContext&) const = default;
};

/** The receipt binds the requested action to one paid source; client clocks grant nothing. */
struct PackageActionContext {
    PurchaseReceipt receipt{};
    NativeOwnership ownership{};
    PackageAction action{};
    std::int8_t bucketSelector{-1};
    bool hasClock{};
    std::uint64_t clock{};
    bool operator==(const PackageActionContext&) const = default;
};

[[nodiscard]] bool prepare_package_action(bool refund,
                                          std::uint64_t instanceSoid,
                                          std::uint16_t itemIndex,
                                          std::int32_t expectedQuantity,
                                          std::int8_t bucketSelector,
                                          bool hasClock,
                                          std::uint64_t clock,
                                          PendingRecordRewardGrant& mutation,
                                          const char*& reason) noexcept;
[[nodiscard]] bool materialize_package_action(const AccountState& current,
                                              const PendingRecordRewardGrant& mutation,
                                              AccountState& after) noexcept;
[[nodiscard]] bool commit_package_action(const PendingRecordRewardGrant& mutation) noexcept;

/** One ownership traversal shared by purchase persistence and outbound flag projection. */
template <typename Visitor>
[[nodiscard]] bool visit_purchase_ownership(const PurchaseContext& context, Visitor&& visitor) noexcept {
    if (context.extraOwnershipCount > context.extraOwnership.size()
        || context.recordOwnership != context.ownership.prepared
        || (!context.ownership.prepared && context.ownership != NativeOwnership{}))
        return false;
    if (context.ownership.prepared && !visitor(context.ownership)) return false;
    for (std::size_t i = 0; i < context.extraOwnership.size(); ++i) {
        const auto& ownership = context.extraOwnership[i];
        if (i >= context.extraOwnershipCount) {
            if (ownership != NativeOwnership{}) return false;
            continue;
        }
        if (!ownership.prepared || ownership.accountFlag == context.ownership.accountFlag)
            return false;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (ownership.accountFlag == context.extraOwnership[prior].accountFlag) return false;
        if (!visitor(ownership)) return false;
    }
    return true;
}

[[nodiscard]] bool is_store_vendor(std::int32_t vendorIndex) noexcept;
[[nodiscard]] bool prepare_purchase(std::int32_t vendorIndex,
                                    std::int32_t saleIndex,
                                    PendingRecordRewardGrant& mutation,
                                    const char*& reason) noexcept;
[[nodiscard]] bool materialize_purchase(const AccountState& current,
                                        const PendingRecordRewardGrant& mutation,
                                        AccountState& after) noexcept;
/** Called by commit_record_reward with its existing mutation-consumption guard. */
[[nodiscard]] bool commit_purchase(const PendingRecordRewardGrant& mutation) noexcept;

} // namespace sunrise::state::eververse
