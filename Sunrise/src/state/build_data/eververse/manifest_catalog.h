#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sunrise::state::build_data::eververse {

/** Metadata from the user's manifest, never a shipped reward or ownership table. */
struct ItemMetadata {
    std::uint32_t itemHash{};
    std::uint16_t itemIndex{};
    std::uint8_t bucketIndex{};
    std::uint8_t recoveryBucketIndex{0xFF};
    std::uint32_t collectibleHash{};
    std::uint32_t plugCategoryHash{};
    std::int32_t maxStackSize{};
    bool instanced{};
    bool unlockAction{};
    bool deleteOnAction{};
    bool useOnAcquire{};
    bool onActionRecreateSelf{};
    bool isDummy{};
    std::uint32_t previewVendorHash{};
    bool isWrapper{};
    bool operator==(const ItemMetadata&) const = default;
};

/** One advertised entry; a display family holds correlated concrete variants, never the dummy. */
struct EngramEntry {
    std::uint32_t advertisedHash{};
    std::int32_t quantity{};
    bool correlatedFamily{};
    std::vector<ItemMetadata> candidates{};
};

struct EngramCatalog {
    ItemMetadata source{};
    std::uint64_t fingerprint{};
    std::vector<EngramEntry> entries{};
};

/**
 * Import user-supplied tables into a private in-memory SQLite database. No source is modified.
 * The directory contains tables/Destiny*Definition.json (or is the tables directory itself).
 * Used by local replays as well as the runtime loader. Failure leaves output empty.
 */
[[nodiscard]] bool load_engram_catalog(const std::filesystem::path& directory,
                                       std::uint32_t sourceHash,
                                       EngramCatalog& output,
                                       std::string& reason) noexcept;

/** Cached for this process; SUNRISE_MANIFEST_DIRECTORY or Sunrise/manifest beside the DLL. */
[[nodiscard]] bool read_engram_catalog(std::uint32_t sourceHash, EngramCatalog& output) noexcept;
[[nodiscard]] bool read_item(std::uint32_t itemHash, ItemMetadata& output) noexcept;
/** Authored Unlock with only source deletion; excludes costs, cooldowns and reward actions. */
[[nodiscard]] bool is_simple_unlock_action(std::uint32_t itemHash) noexcept;
/** Wrapper Open with no separately authored costs, rewards or automatic acquisition action. */
[[nodiscard]] bool is_simple_wrapper_action(std::uint32_t itemHash) noexcept;

/** Source-bound Sunrise final-purchase expansion; reward identities come from local content. */
inline constexpr std::uint32_t kArrivalsStarterPackHash = 1644330125U;
struct StarterPack {
    struct Entry {
        ItemMetadata advertised{};
        std::int32_t quantity{};
    };
    std::array<Entry, 6> entries{};
    std::uint16_t previewVendorIndex{};
};
/** Reads every guaranteed preview row; random categories and incomplete lists are refused. */
[[nodiscard]] bool read_starter_pack(StarterPack& output) noexcept;

/**
 * One supported direct advertised entry with its preview quantity. State supplies eligible
 * identities after checking native routing and account ownership. Display families and
 * on-acquire/recreate-self actions remain excluded. Stable account/source identities determine
 * the draw over that eligible pool; no retail weights or accompanying reward are inferred.
 */
inline constexpr char kEngramPolicy[] = "SunriseDirectPreviewUnlockProtectionV2";
[[nodiscard]] bool selectable_engram_entry(const EngramEntry& entry) noexcept;
struct EngramSelection {
    ItemMetadata item{};
    std::uint32_t advertisedHash{};
    std::int32_t quantity{};
    std::uint64_t catalogFingerprint{};
    bool correlatedFamily{};
    bool operator==(const EngramSelection&) const = default;
};
[[nodiscard]] bool select_engram_reward(const EngramCatalog& catalog,
                                        std::uint64_t accountSoid,
                                        std::uint64_t sourceSoid,
                                        std::span<const std::uint32_t> eligibleHashes,
                                        EngramSelection& output) noexcept;

} // namespace sunrise::state::build_data::eververse
