#include "store_internal.h"

namespace sunrise::state::investment::store {

sqlite3* g_database{};
std::recursive_mutex g_mutex;
Session g_session;
std::uint64_t g_failureSerial{};

/** SQLite requires this sentinel to copy text borrowed from a caller. */
sqlite3_destructor_type copy_text() noexcept {
    return SQLITE_TRANSIENT; // NOLINT(performance-no-int-to-ptr)
}

/** SQL failures remain failures; no in-memory save replaces a failed disk write. */
bool execute(const char* sql) noexcept {
    const bool ready = g_database != nullptr
                       && sqlite3_exec(g_database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ready) {
        ++g_failureSerial;
    }
    return ready;
}

/** Nested saves belong to the outer transaction until it commits. */
Transaction::Transaction() noexcept
    : lock_(g_mutex), active_(execute("SAVEPOINT investment_write")), before_(g_session),
      failureSerial_(g_failureSerial) {}

/** A refused transaction leaves no partial rows. */
Transaction::~Transaction() {
    if (active_) {
        (void)execute("ROLLBACK TO investment_write");
        (void)execute("RELEASE investment_write");
        g_session = before_;
    }
}

/** @return True only after SQLite has committed every row. */
bool Transaction::commit() noexcept {
    if (!active_ || failureSerial_ != g_failureSerial || !execute("RELEASE investment_write")) {
        return false;
    }
    active_ = false;
    return true;
}

/** The caller holds the database lock until this statement is destroyed. */
Statement::Statement(const char* sql) noexcept {
    if (g_database != nullptr
        && sqlite3_prepare_v2(g_database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
        sqlite3_finalize(statement_);
        statement_ = nullptr;
        ++g_failureSerial;
    }
}

Statement::~Statement() {
    sqlite3_finalize(statement_);
}
int Statement::step() noexcept {
    const int result = statement_ != nullptr ? sqlite3_step(statement_) : SQLITE_ERROR;
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        ++g_failureSerial;
    }
    return result;
}

/** @return Borrowed text valid until the statement advances. */
bool Statement::text(int column, std::string_view& value) const noexcept {
    if (statement_ == nullptr || sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
    if (bytes == nullptr) {
        return false;
    }
    value = {bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
    return true;
}

static_assert(account::inventory::kCharacterItemCapacity == 333);
static_assert(account::inventory::kCharacterStackCapacity == 350);

/** Widen saved row bounds to the character ABI and preserve socket identities during migration. */
static bool migrate_postmaster_storage() noexcept {
    // SQLite cannot change foreign_keys inside a savepoint. The open-time mutex is held and
    // no account readers exist yet; restore enforcement after the savepoint ends on either path.
    if (!execute("PRAGMA foreign_keys=OFF")) {
        return false;
    }
    const bool migrated = []() noexcept {
        Transaction transaction;
        if (!transaction.ready() || !execute(R"sql(
CREATE TABLE items_postmaster_migration (
    character_slot INTEGER NOT NULL REFERENCES characters(slot),
    location INTEGER NOT NULL CHECK (location IN (0, 1)),
    position INTEGER NOT NULL CHECK (position >= 0),
    instance_soid INTEGER NOT NULL UNIQUE,
    definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),
    level INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    flags INTEGER NOT NULL CHECK (flags BETWEEN 0 AND 7),
    socket_policy INTEGER NOT NULL CHECK (socket_policy IN (0, 1)),
    plug_count INTEGER NOT NULL CHECK (plug_count BETWEEN 0 AND 12),
    movement_ability INTEGER NOT NULL,
    grenade_ability INTEGER NOT NULL,
    super_ability INTEGER NOT NULL,
    melee_ability INTEGER NOT NULL,
    class_ability INTEGER NOT NULL,
    seen INTEGER NOT NULL CHECK (seen IN (0, 1)),
    placement INTEGER NOT NULL DEFAULT 0 CHECK (placement IN (0, 1)),
    PRIMARY KEY (character_slot, location, position),
    CHECK ((location = 0 AND position < 17 AND placement = 0)
           OR (location = 1 AND position < 333)),
    CHECK (placement = 0 OR quantity = 1)
) STRICT;
INSERT INTO items_postmaster_migration SELECT items.*, 0 FROM items;
DROP TABLE items;
ALTER TABLE items_postmaster_migration RENAME TO items;
CREATE TABLE character_stacks_migration (
    character_slot INTEGER NOT NULL REFERENCES characters(slot),
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 349),
    definition_hash INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    PRIMARY KEY (character_slot, position)
) STRICT;
INSERT INTO character_stacks_migration SELECT * FROM character_stacks;
DROP TABLE character_stacks;
ALTER TABLE character_stacks_migration RENAME TO character_stacks;
PRAGMA user_version=3;
)sql")) {
            return false;
        }
        Statement foreignKeys("PRAGMA foreign_key_check");
        return foreignKeys.step() == SQLITE_DONE && transaction.commit();
    }();
    const bool enforced = execute("PRAGMA foreign_keys=ON");
    return migrated && enforced;
}

/** New databases receive schema and defaults in one durable transaction. */
bool open(std::string_view path,
          std::string_view schema,
          std::string_view defaults,
          std::string_view settingsSchema,
          std::string_view settingsDefaults) noexcept {
    const std::lock_guard lock(g_mutex);
    if (g_database != nullptr) {
        return false;
    }
    const std::string filename(path);
    if (sqlite3_open_v2(filename.c_str(),
                        &g_database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr)
        != SQLITE_OK) {
        shutdown();
        return false;
    }
    // One short busy wait permits a local database editor to finish its transaction.
    constexpr int kBusyMilliseconds = 5000;
    sqlite3_busy_timeout(g_database, kBusyMilliseconds);
    bool ready = execute("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
    int version = -1;
    {
        Statement query("PRAGMA user_version");
        ready = ready && query.step() == SQLITE_ROW && query.column(0, version);
    }
    const std::string preferenceSchema(settingsSchema);
    const std::string preferenceDefaults(settingsDefaults);
    if (ready && version == 0) {
        Transaction transaction;
        const std::string schemaText(schema);
        const std::string defaultText(defaults);
        ready = transaction.ready() && !schema.empty() && !defaults.empty()
                && execute(schemaText.c_str()) && execute(defaultText.c_str())
                && !settingsSchema.empty() && !settingsDefaults.empty()
                && execute(preferenceSchema.c_str()) && execute(preferenceDefaults.c_str())
                && transaction.commit();
    } else if (ready) {
        // Version 3 adds saved item placement and room for Lost Items.
        constexpr int kSchemaVersion = 3;
        constexpr int kApplicationId = 1397902921;
        int application = 0;
        Statement query("PRAGMA application_id");
        ready = (version >= 1 && version <= kSchemaVersion) && query.step() == SQLITE_ROW
                && query.column(0, application) && application == kApplicationId;
    }
    if (ready && version == 1) {
        Transaction transaction;
        ready =
            transaction.ready() && !settingsSchema.empty() && !settingsDefaults.empty()
            && execute(
                "ALTER TABLE items ADD COLUMN seen INTEGER NOT NULL DEFAULT 0 CHECK(seen IN(0,1));"
                "ALTER TABLE profile_items ADD COLUMN seen INTEGER NOT NULL DEFAULT 1 CHECK(seen "
                "IN(0,1));")
            && execute(preferenceSchema.c_str()) && execute(preferenceDefaults.c_str())
            && execute("PRAGMA user_version=2") && transaction.commit();
    }
    if (ready) {
        int currentVersion = 0;
        {
            Statement query("PRAGMA user_version");
            ready = query.step() == SQLITE_ROW && query.column(0, currentVersion);
        }
        if (ready && currentVersion == 2) {
            ready = migrate_postmaster_storage();
        }
    }

    if (!ready) {
        shutdown();
    }
    return ready;
}

/** Saves are synchronous, so shutdown only closes the handle and discards session fields. */
void shutdown() noexcept {
    const std::lock_guard lock(g_mutex);
    sqlite3_close_v2(g_database);
    g_database = nullptr;
    g_session = {};
}

} // namespace sunrise::state::investment::store
