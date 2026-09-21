#include "manifest_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "../../../../vendor/sqlite/sqlite3.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"

namespace sunrise::state::build_data::eververse {
namespace {
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
constexpr std::size_t kEntryLimit = 4096;
constexpr std::size_t kFileLimit = 256 * 1024 * 1024;

[[nodiscard]] Statement prepare(sqlite3* db, const char* sql) noexcept {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_finalize(statement);
        statement = nullptr;
    }
    return {statement, sqlite3_finalize};
}

[[nodiscard]] bool execute(sqlite3* db, const char* sql) noexcept {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

/** The import only retains JSON from the local file; all schema names below are program-owned. */
[[nodiscard]] bool import_table(sqlite3* db,
                                const std::filesystem::path& directory,
                                const char* file,
                                const char* sql) {
    std::ifstream input(directory / file, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kFileLimit)) return false;
    std::string json(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size()))) return false;
    auto statement = prepare(db, sql);
    return statement
           && sqlite3_bind_text(
                  statement.get(), 1, json.data(), static_cast<int>(json.size()), SQLITE_STATIC)
                  == SQLITE_OK
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

[[nodiscard]] Database open_manifest(std::filesystem::path directory) {
    if (std::filesystem::is_directory(directory / "tables")) directory /= "tables";
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(":memory:",
                                       &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                           | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOMUTEX,
                                       nullptr);
    Database db(raw, sqlite3_close);
    if (opened != SQLITE_OK) return {nullptr, sqlite3_close};
    sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, static_cast<int>(kFileLimit));
    if (!execute(db.get(), R"sql(
        CREATE TABLE items(hash INTEGER PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE vendors(hash INTEGER PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE buckets(hash INTEGER PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE perks(hash INTEGER PRIMARY KEY, data TEXT NOT NULL);
    )sql")
        || !import_table(
            db.get(),
            directory,
            "DestinyInventoryItemDefinition.json",
            "INSERT INTO items SELECT json_extract(value,'$.hash'),value FROM json_each(?1)")
        || !import_table(
            db.get(),
            directory,
            "DestinyVendorDefinition.json",
            "INSERT INTO vendors SELECT json_extract(value,'$.hash'),value FROM json_each(?1)")
        || !import_table(
            db.get(),
            directory,
            "DestinyInventoryBucketDefinition.json",
            "INSERT INTO buckets SELECT json_extract(value,'$.hash'),value FROM json_each(?1)")
        || !import_table(
            db.get(),
            directory,
            "DestinySandboxPerkDefinition.json",
            "INSERT INTO perks SELECT json_extract(value,'$.hash'),value FROM json_each(?1)")) {
        return {nullptr, sqlite3_close};
    }
    // Materializing the relevant fields avoids reparsing the entire item table per family.
    if (!execute(db.get(), R"sql(
        CREATE TABLE metadata AS SELECT i.hash,
          json_extract(i.data,'$.index') AS itemIndex,
          json_extract(b.data,'$.index') AS bucketIndex,
          json_extract(r.data,'$.index') AS recoveryBucketIndex,
          coalesce(json_extract(i.data,'$.collectibleHash'),0) AS collectibleHash,
          coalesce(json_extract(i.data,'$.plug.plugCategoryHash'),0) AS plugCategoryHash,
          json_extract(i.data,'$.inventory.maxStackSize') AS maxStackSize,
          json_extract(i.data,'$.inventory.isInstanceItem') AS instanced,
          coalesce(json_extract(i.data,'$.action.verbName')='Unlock',0) AS unlockAction,
          coalesce(json_extract(i.data,'$.action.deleteOnAction'),0) AS deleteOnAction,
          coalesce(json_extract(i.data,'$.action.useOnAcquire'),0) AS useOnAcquire,
          coalesce(json_extract(i.data,'$.plug.onActionRecreateSelf'),0) AS onActionRecreateSelf,
          coalesce(json_extract(i.data,'$.itemType')=20,0)
            OR coalesce(json_extract(i.data,'$.plug.isDummyPlug'),0) AS isDummy,
          coalesce(json_extract(i.data,'$.preview.previewVendorHash'),0) AS previewVendorHash,
          coalesce(json_extract(i.data,'$.isWrapper'),0) AS isWrapper
        FROM items i
        LEFT JOIN buckets b ON b.hash=json_extract(i.data,'$.inventory.bucketTypeHash')
        LEFT JOIN buckets r ON r.hash=json_extract(i.data,'$.inventory.recoveryBucketTypeHash');
        CREATE UNIQUE INDEX metadata_hash ON metadata(hash);
        CREATE TABLE effect_icons AS
        SELECT DISTINCT i.hash,json_extract(p.data,'$.displayProperties.icon') AS icon
        FROM items i,json_each(i.data,'$.perks') ip
        JOIN perks p ON p.hash=json_extract(ip.value,'$.perkHash')
        WHERE json_extract(i.data,'$.plug.plugCategoryIdentifier')='ship.spawnfx'
          AND json_extract(p.data,'$.displayProperties.icon') IS NOT NULL;
        CREATE INDEX effect_icon ON effect_icons(icon);
    )sql"))
        return {nullptr, sqlite3_close};
    return db;
}

[[nodiscard]] bool integer(sqlite3_stmt* statement,
                           int column,
                           std::int64_t minimum,
                           std::int64_t maximum,
                           std::int64_t& result) noexcept {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) return false;
    result = sqlite3_column_int64(statement, column);
    return result >= minimum && result <= maximum;
}

[[nodiscard]] bool item_metadata(sqlite3* db, std::uint32_t hash, ItemMetadata& output) noexcept {
    output = {};
    auto statement = prepare(db, "SELECT * FROM metadata WHERE hash=?1");
    if (!statement || sqlite3_bind_int64(statement.get(), 1, hash) != SQLITE_OK
        || sqlite3_step(statement.get()) != SQLITE_ROW)
        return false;
    std::array<std::int64_t, 15> fields{};
    constexpr std::array<std::int64_t, 15> maximums{4294967295LL,
                                                    32767,
                                                    255,
                                                    255,
                                                    4294967295LL,
                                                    4294967295LL,
                                                    2147483647,
                                                    1,
                                                    1,
                                                    1,
                                                    1,
                                                    1,
                                                    1,
                                                    4294967295LL,
                                                    1};
    for (int column = 0; column < static_cast<int>(fields.size()); ++column) {
        if (column == 3 && sqlite3_column_type(statement.get(), column) == SQLITE_NULL) {
            fields[column] = 0xFF;
            continue;
        }
        if (!integer(statement.get(),
                     column,
                     column == 0 || column == 6 ? 1 : 0,
                     maximums[column],
                     fields[column]))
            return false;
    }
    output = {static_cast<std::uint32_t>(fields[0]),
              static_cast<std::uint16_t>(fields[1]),
              static_cast<std::uint8_t>(fields[2]),
              static_cast<std::uint8_t>(fields[3]),
              static_cast<std::uint32_t>(fields[4]),
              static_cast<std::uint32_t>(fields[5]),
              static_cast<std::int32_t>(fields[6]),
              fields[7] != 0,
              fields[8] != 0,
              fields[9] != 0,
              fields[10] != 0,
              fields[11] != 0,
              fields[12] != 0,
              static_cast<std::uint32_t>(fields[13]),
              fields[14] != 0};
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

void fingerprint_item(std::uint64_t& value, const ItemMetadata& item) noexcept {
    const std::array<std::uint64_t, 15> fields{item.itemHash,
                                               item.itemIndex,
                                               item.bucketIndex,
                                               item.recoveryBucketIndex,
                                               item.collectibleHash,
                                               item.plugCategoryHash,
                                               static_cast<std::uint64_t>(item.maxStackSize),
                                               item.instanced,
                                               item.unlockAction,
                                               item.deleteOnAction,
                                               item.useOnAcquire,
                                               item.onActionRecreateSelf,
                                               item.isDummy,
                                               item.previewVendorHash,
                                               item.isWrapper};
    for (auto field : fields)
        value = mix(value ^ field);
}

[[nodiscard]] bool
build_catalog(sqlite3* db, std::uint32_t sourceHash, EngramCatalog& output, std::string& reason) {
    EngramCatalog candidate{};
    reason = "source_sack";
    if (!item_metadata(db, sourceHash, candidate.source) || candidate.source.isDummy
        || !candidate.source.instanced || candidate.source.maxStackSize != 1)
        return false;
    auto source = prepare(db, R"sql(
        SELECT p.hash FROM items i JOIN vendors p
          ON p.hash=json_extract(i.data,'$.preview.previewVendorHash')
        WHERE i.hash=?1 AND json_extract(i.data,'$.sack.vendorSackType')='engram.silver'
          AND json_extract(i.data,'$.sack.openOnAcquire')=0
          AND json_extract(p.data,'$.inhibitBuying')=1
          AND json_extract(p.data,'$.inhibitSelling')=1
    )sql");
    if (!source || sqlite3_bind_int64(source.get(), 1, sourceHash) != SQLITE_OK
        || sqlite3_step(source.get()) != SQLITE_ROW)
        return false;
    const auto previewHash = sqlite3_column_int64(source.get(), 0);
    if (sqlite3_step(source.get()) != SQLITE_DONE) return false;
    // A broken join must not quietly shrink the candidate pool. Validate every parent row,
    // including featured rows, and every nonzero child preview reference before traversing.
    auto links = prepare(db, R"sql(
        SELECT count(*) FROM vendors parent,json_each(parent.data,'$.itemList') entry
        LEFT JOIN items i ON i.hash=json_extract(entry.value,'$.itemHash')
        LEFT JOIN vendors child ON child.hash=json_extract(i.data,'$.preview.previewVendorHash')
        WHERE parent.hash=?1 AND
          (i.hash IS NULL OR
           (coalesce(json_extract(i.data,'$.preview.previewVendorHash'),0)<>0 AND child.hash IS NULL))
    )sql");
    reason = "preview_links";
    if (!links || sqlite3_bind_int64(links.get(), 1, previewHash) != SQLITE_OK
        || sqlite3_step(links.get()) != SQLITE_ROW || sqlite3_column_int64(links.get(), 0) != 0
        || sqlite3_step(links.get()) != SQLITE_DONE)
        return false;
    // Only rows linked through category selectors are candidates. Featured display entries
    // are not a second reward list and cannot introduce a currency proxy into the pool.
    auto entries = prepare(db, R"sql(
        SELECT json_extract(reward.value,'$.itemHash'),json_extract(reward.value,'$.quantity'),
               json_extract(child.data,'$.inhibitBuying'),json_extract(child.data,'$.inhibitSelling')
        FROM vendors parent,json_each(parent.data,'$.itemList') selector
        JOIN items s ON s.hash=json_extract(selector.value,'$.itemHash')
        JOIN vendors child ON child.hash=json_extract(s.data,'$.preview.previewVendorHash'),
             json_each(child.data,'$.itemList') reward
        WHERE parent.hash=?1
        ORDER BY json_extract(reward.value,'$.itemHash')
    )sql");
    reason = "preview_entries";
    if (!entries || sqlite3_bind_int64(entries.get(), 1, previewHash) != SQLITE_OK) return false;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(entries.get())) == SQLITE_ROW) {
        std::int64_t hash = 0, quantity = 0, inhibited = 0;
        if (candidate.entries.size() == kEntryLimit
            || !integer(entries.get(), 0, 1, 4294967295LL, hash)
            || !integer(entries.get(), 1, 1, 2147483647LL, quantity)
            || !integer(entries.get(), 2, 1, 1, inhibited)
            || !integer(entries.get(), 3, 1, 1, inhibited))
            return false;
        EngramEntry entry{};
        entry.advertisedHash = static_cast<std::uint32_t>(hash);
        entry.quantity = static_cast<std::int32_t>(quantity);
        if (!candidate.entries.empty()
            && candidate.entries.back().advertisedHash == entry.advertisedHash)
            return false;
        ItemMetadata advertised{};
        if (!item_metadata(db, entry.advertisedHash, advertised)) return false;
        entry.correlatedFamily = advertised.isDummy;
        if (!entry.correlatedFamily) {
            if (advertised.recoveryBucketIndex == 0xFF || entry.quantity > advertised.maxStackSize)
                return false;
            entry.candidates.push_back(advertised);
        } else {
            // Shared first-perk icon is a documented correlation, not a reward foreign key.
            // Never use a display name substring, and never include the display dummy itself.
            auto variants = prepare(db, R"sql(
                SELECT DISTINCT e.hash FROM items dummy
                JOIN perks p ON p.hash=json_extract(dummy.data,'$.perks[0].perkHash')
                JOIN effect_icons e ON e.icon=json_extract(p.data,'$.displayProperties.icon')
                WHERE dummy.hash=?1 ORDER BY e.hash
            )sql");
            reason = "family_variants";
            if (!variants || sqlite3_bind_int64(variants.get(), 1, hash) != SQLITE_OK) return false;
            int variantStep = SQLITE_DONE;
            while ((variantStep = sqlite3_step(variants.get())) == SQLITE_ROW) {
                ItemMetadata concrete{};
                std::int64_t variant = 0;
                if (entry.candidates.size() == kEntryLimit
                    || !integer(variants.get(), 0, 1, 4294967295LL, variant)
                    || !item_metadata(db, static_cast<std::uint32_t>(variant), concrete)
                    || concrete.isDummy || concrete.collectibleHash == 0
                    || concrete.plugCategoryHash == 0 || concrete.recoveryBucketIndex == 0xFF
                    || entry.quantity > concrete.maxStackSize)
                    return false;
                entry.candidates.push_back(concrete);
            }
            if (variantStep != SQLITE_DONE || entry.candidates.empty()) return false;
        }
        candidate.entries.push_back(std::move(entry));
    }
    if (step != SQLITE_DONE || candidate.entries.empty()) return false;
    candidate.fingerprint = 1; // Version of the extraction/selection contract, not game data.
    fingerprint_item(candidate.fingerprint, candidate.source);
    for (const auto& entry : candidate.entries) {
        candidate.fingerprint = mix(candidate.fingerprint ^ entry.advertisedHash);
        candidate.fingerprint =
            mix(candidate.fingerprint ^ static_cast<std::uint64_t>(entry.quantity));
        for (const auto& item : entry.candidates)
            fingerprint_item(candidate.fingerprint, item);
    }
    if (candidate.fingerprint == 0) return false;
    output = std::move(candidate);
    reason.clear();
    return true;
}

struct RuntimeCatalog {
    std::mutex mutex{};
    Database database{nullptr, sqlite3_close};
    std::vector<EngramCatalog> engrams{};
    bool attempted{};
};
RuntimeCatalog g_runtime{};

[[nodiscard]] bool runtime_database() {
    if (g_runtime.attempted) return g_runtime.database != nullptr;
    g_runtime.attempted = true;
    core::path::Buffer path{};
    const auto length = GetEnvironmentVariableW(
        L"SUNRISE_MANIFEST_DIRECTORY", path.chars.data(), static_cast<DWORD>(path.chars.size()));
    if (length >= path.chars.size()) return false;
    if (length == 0 && !core::path::artifact_file(L"manifest", path)) return false;
    g_runtime.database = open_manifest(std::filesystem::path(path.chars.data()));
    if (!g_runtime.database) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bright_engram stage=manifest result=unavailable source=local_tables");
    }
    return g_runtime.database != nullptr;
}
} // namespace

bool load_engram_catalog(const std::filesystem::path& directory,
                         std::uint32_t sourceHash,
                         EngramCatalog& output,
                         std::string& reason) noexcept {
    output = {};
    try {
        reason = "manifest_tables";
        auto db = open_manifest(directory);
        return db && build_catalog(db.get(), sourceHash, output, reason);
    } catch (...) {
        output = {};
        return false;
    }
}

bool read_engram_catalog(std::uint32_t sourceHash, EngramCatalog& output) noexcept {
    output = {};
    try {
        const std::lock_guard lock(g_runtime.mutex);
        std::string reason;
        if (!runtime_database()) return false;
        for (const auto& catalog : g_runtime.engrams) {
            if (catalog.source.itemHash == sourceHash) {
                output = catalog;
                return true;
            }
        }
        if (g_runtime.engrams.size() == 32) return false;
        EngramCatalog catalog{};
        if (!build_catalog(g_runtime.database.get(), sourceHash, catalog, reason)) return false;
        g_runtime.engrams.push_back(std::move(catalog));
        output = g_runtime.engrams.back();
        return true;
    } catch (...) {
        return false;
    }
}

bool read_item(std::uint32_t hash, ItemMetadata& output) noexcept {
    output = {};
    try {
        const std::lock_guard lock(g_runtime.mutex);
        return runtime_database() && item_metadata(g_runtime.database.get(), hash, output);
    } catch (...) {
        return false;
    }
}

bool read_starter_pack(StarterPack& output) noexcept {
    output = {};
    try {
        const std::lock_guard lock(g_runtime.mutex);
        if (!runtime_database()) return false;
        auto* db = g_runtime.database.get();
        auto source = prepare(db, R"sql(
            SELECT json_extract(v.data,'$.index')
            FROM items s JOIN vendors v ON v.hash=json_extract(s.data,'$.preview.previewVendorHash')
            WHERE s.hash=?1 AND v.hash=s.hash
              AND json_extract(s.data,'$.index')=12354 AND json_extract(s.data,'$.itemType')=25
              AND json_type(s.data,'$.action') IS NULL
              AND json_extract(v.data,'$.inhibitBuying')=1
              AND json_extract(v.data,'$.inhibitSelling')=1
              AND json_type(v.data,'$.itemList')='array'
              AND json_array_length(v.data,'$.itemList')=6
              AND json_type(s.data,'$.preview.derivedItemCategories')='array'
              AND json_array_length(s.data,'$.preview.derivedItemCategories')=1
              AND json_type(s.data,'$.preview.derivedItemCategories[0].items')='array'
              AND json_array_length(s.data,'$.preview.derivedItemCategories[0].items')=6
        )sql");
        std::int64_t vendorIndex{};
        if (!source || sqlite3_bind_int64(source.get(), 1, kArrivalsStarterPackHash) != SQLITE_OK
            || sqlite3_step(source.get()) != SQLITE_ROW
            || !integer(source.get(), 0, 0, 32767, vendorIndex)
            || sqlite3_step(source.get()) != SQLITE_DONE)
            return false;
        auto rows = prepare(db, R"sql(
            SELECT json_extract(r.value,'$.itemHash'),json_extract(r.value,'$.quantity'),
                   json_extract(r.value,'$.vendorItemIndex'),
                   CASE WHEN json_type(r.value,'$.currencies')='array'
                     AND json_array_length(r.value,'$.currencies')=0
                     AND json_extract(r.value,'$.refundPolicy')=0
                     AND (SELECT count(*) FROM json_each(v.data,'$.displayCategories') c
                          WHERE json_extract(c.value,'$.index')=json_extract(r.value,'$.displayCategoryIndex')
                            AND json_extract(c.value,'$.displayCategoryHash')=715509015
                            AND json_extract(c.value,'$.identifier')='category_guaranteed_contents')=1
                     AND (SELECT count(*) FROM json_each(s.data,'$.preview.derivedItemCategories[0].items') d
                          WHERE json_extract(d.value,'$.vendorItemIndex')=json_extract(r.value,'$.vendorItemIndex')
                            AND json_extract(d.value,'$.itemHash')=json_extract(r.value,'$.itemHash'))=1
                   THEN 1 ELSE 0 END
            FROM vendors v JOIN items s ON s.hash=v.hash,json_each(v.data,'$.itemList') r
            WHERE v.hash=?1 ORDER BY json_extract(r.value,'$.vendorItemIndex')
        )sql");
        if (!rows || sqlite3_bind_int64(rows.get(), 1, kArrivalsStarterPackHash) != SQLITE_OK)
            return false;
        StarterPack candidate{};
        candidate.previewVendorIndex = static_cast<std::uint16_t>(vendorIndex);
        for (std::size_t i = 0; i < candidate.entries.size(); ++i) {
            std::int64_t hash{}, quantity{}, rowIndex{}, guaranteed{};
            if (sqlite3_step(rows.get()) != SQLITE_ROW
                || !integer(rows.get(), 0, 1, 4294967295LL, hash)
                || !integer(rows.get(), 1, 1, 2147483647LL, quantity)
                || !integer(rows.get(), 2, 0, 5, rowIndex) || rowIndex != static_cast<std::int64_t>(i)
                || !integer(rows.get(), 3, 1, 1, guaranteed))
                return false;
            auto& entry = candidate.entries[i];
            if (!item_metadata(db, static_cast<std::uint32_t>(hash), entry.advertised)) return false;
            for (std::size_t prior = 0; prior < i; ++prior)
                if (candidate.entries[prior].advertised.itemHash == entry.advertised.itemHash)
                    return false;
            entry.quantity = static_cast<std::int32_t>(quantity);
        }
        if (sqlite3_step(rows.get()) != SQLITE_DONE) return false;
        output = candidate;
        return true;
    } catch (...) {
        return false;
    }
}

bool is_simple_unlock_action(std::uint32_t itemHash) noexcept {
    try {
        const std::lock_guard lock(g_runtime.mutex);
        if (!runtime_database()) return false;
        auto row = prepare(g_runtime.database.get(), R"sql(
            SELECT 1 FROM items WHERE hash=?1
              AND json_extract(data,'$.action.verbName')='Unlock'
              AND json_extract(data,'$.action.deleteOnAction')=1
              AND json_extract(data,'$.action.consumeEntireStack')=0
              AND json_extract(data,'$.action.useOnAcquire')=0
              AND json_extract(data,'$.action.requiredCooldownSeconds')=0
              AND json_extract(data,'$.action.requiredCooldownHash')=0
              AND json_type(data,'$.action.requiredItems')='array'
              AND json_array_length(data,'$.action.requiredItems')=0
              AND json_type(data,'$.action.progressionRewards')='array'
              AND json_array_length(data,'$.action.progressionRewards')=0
              AND json_extract(data,'$.action.rewardSheetHash')=0
              AND json_extract(data,'$.action.rewardItemHash')=0
              AND json_extract(data,'$.action.rewardSiteHash')=0
              AND json_extract(data,'$.plug.onActionRecreateSelf')=0
              AND json_extract(data,'$.plug.actionRewardSiteHash')=0
              AND json_extract(data,'$.plug.actionRewardItemOverrideHash')=0
        )sql");
        return row && sqlite3_bind_int64(row.get(), 1, itemHash) == SQLITE_OK
               && sqlite3_step(row.get()) == SQLITE_ROW
               && sqlite3_step(row.get()) == SQLITE_DONE;
    } catch (...) {
        return false;
    }
}

bool is_simple_wrapper_action(std::uint32_t itemHash) noexcept {
    try {
        const std::lock_guard lock(g_runtime.mutex);
        if (!runtime_database()) return false;
        auto row = prepare(g_runtime.database.get(), R"sql(
            SELECT 1 FROM items WHERE hash=?1
              AND json_extract(data,'$.isWrapper')=1
              AND json_extract(data,'$.action.verbName')='Open'
              AND json_extract(data,'$.action.isPositive')=1
              AND json_extract(data,'$.action.deleteOnAction')=0
              AND json_extract(data,'$.action.consumeEntireStack')=0
              AND json_extract(data,'$.action.useOnAcquire')=0
              AND json_extract(data,'$.action.requiredCooldownSeconds')=0
              AND json_extract(data,'$.action.requiredCooldownHash')=0
              AND json_type(data,'$.action.requiredItems')='array'
              AND json_array_length(data,'$.action.requiredItems')=0
              AND json_type(data,'$.action.progressionRewards')='array'
              AND json_array_length(data,'$.action.progressionRewards')=0
              AND json_extract(data,'$.action.rewardSheetHash')=0
              AND json_extract(data,'$.action.rewardItemHash')=0
              AND json_extract(data,'$.action.rewardSiteHash')=0
              AND coalesce(json_type(data,'$.plug'),'null')='null'
        )sql");
        return row && sqlite3_bind_int64(row.get(), 1, itemHash) == SQLITE_OK
               && sqlite3_step(row.get()) == SQLITE_ROW
               && sqlite3_step(row.get()) == SQLITE_DONE;
    } catch (...) {
        return false;
    }
}

bool selectable_engram_entry(const EngramEntry& entry) noexcept {
    if (entry.correlatedFamily || entry.candidates.size() != 1) return false;
    const auto& item = entry.candidates.front();
    // State validates the concrete Unlock action and filters prior acquisitions before drawing.
    // Instanced Unlock deliveries (such as the emote collection) need a separate native route.
    const bool supportedAction = !item.unlockAction
        || (!item.instanced && item.deleteOnAction && entry.quantity == 1);
    return supportedAction && !item.isDummy && !item.isWrapper && item.previewVendorHash == 0
           && !item.onActionRecreateSelf && !item.useOnAcquire && item.itemHash != 0
           && entry.quantity > 0 && entry.quantity <= item.maxStackSize;
}

bool select_engram_reward(const EngramCatalog& catalog,
                          std::uint64_t accountSoid,
                          std::uint64_t sourceSoid,
                          std::span<const std::uint32_t> eligibleHashes,
                          EngramSelection& output) noexcept {
    output = {};
    if (!accountSoid || !sourceSoid || !catalog.fingerprint || catalog.entries.empty())
        return false;
    // Capacity never rerolls a draw. State revalidates this same ownership-qualified pool
    // before publishing/committing, so a stale pending reward cannot bypass protection.
    const auto eligible = [&](const EngramEntry& entry) {
        return selectable_engram_entry(entry)
            && std::find(eligibleHashes.begin(), eligibleHashes.end(),
                         entry.candidates.front().itemHash) != eligibleHashes.end();
    };
    const auto seed = mix(sourceSoid ^ mix(accountSoid) ^ mix(catalog.source.itemHash));
    const auto count = static_cast<std::size_t>(
        std::count_if(catalog.entries.begin(), catalog.entries.end(), eligible));
    if (count == 0) return false;
    auto ordinal = seed % count;
    for (const auto& entry : catalog.entries) {
        if (!eligible(entry)) continue;
        if (ordinal-- != 0) continue;
        output = {entry.candidates.front(),
                  entry.advertisedHash,
                  entry.quantity,
                  catalog.fingerprint,
                  false};
        return true;
    }
    return false;
}
} // namespace sunrise::state::build_data::eververse
