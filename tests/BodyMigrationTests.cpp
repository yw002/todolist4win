// Version 1 -> 2 migration tests. Everything runs against a temporary database file that is built
// by hand as a genuine v1 database, then upgraded by the real SqliteSchema::EnsureDatabaseAndSchema.
// No SQL is executed against the user's data, no database service is contacted, and the temp directory
// is removed at the end.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <windows.h>

#include <sqlite3.h>

#include "common/Error.h"
#include "data/SqliteConnection.h"
#include "data/SqliteSchema.h"
#include "data/SqliteUnitOfWork.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") { ReportTest("BodyMigration", name, value, detail); }

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_migration_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" +
                             std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void EnsureParent(const std::filesystem::path& path) {
    // The production schema helper creates the data directory; a hand-built v1 fixture does not go
    // through it, so do it here or sqlite3_open_v2 fails with CANTOPEN.
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
}

bool Exec(sqlite3* db, const std::string& sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    if (message != nullptr) sqlite3_free(message);
    return rc == SQLITE_OK;
}

std::int64_t Scalar(sqlite3* db, const std::string& sql) {
    SqliteStatement statement(db, sql);
    if (!statement.Prepare().ok()) return -1;
    auto row = statement.Step();
    if (!row.ok() || !row.value()) return -1;
    return statement.ColumnInt64(0);
}

std::string Text(sqlite3* db, const std::string& sql) {
    SqliteStatement statement(db, sql);
    if (!statement.Prepare().ok()) return {};
    auto row = statement.Step();
    if (!row.ok() || !row.value()) return {};
    return statement.ColumnText(0);
}

// The exact v1 table shape, so CREATE TABLE IF NOT EXISTS during the upgrade is a no-op for these
// four tables and the ADD COLUMN path is the only change to `task`.
constexpr const char* kV1Ddl[] = {
    "CREATE TABLE schema_migration (version INTEGER NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);",
    "CREATE TABLE category (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE, color TEXT NOT NULL, created_at TEXT NOT NULL, CONSTRAINT uq_category_name UNIQUE (name));",
    "CREATE TABLE task (id INTEGER PRIMARY KEY AUTOINCREMENT, category_id INTEGER NULL, title TEXT NOT NULL, note TEXT NOT NULL, priority TEXT NOT NULL, due_time TEXT NULL, completed INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL, completed_at TEXT NULL, sort_weight REAL NOT NULL, revision INTEGER NOT NULL DEFAULT 1, CONSTRAINT fk_task_category FOREIGN KEY (category_id) REFERENCES category(id) ON DELETE RESTRICT ON UPDATE RESTRICT, CONSTRAINT ck_task_priority CHECK (priority IN ('HIGH','MEDIUM','LOW')), CONSTRAINT ck_task_completed CHECK (completed IN (0,1)), CONSTRAINT ck_task_completion_time CHECK ((completed = 0 AND completed_at IS NULL) OR (completed = 1 AND completed_at IS NOT NULL)));",
    "CREATE TABLE history (id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL, title TEXT NOT NULL, note TEXT NOT NULL, priority TEXT NOT NULL, due_time TEXT NULL, original_category_id INTEGER NULL, original_category_name TEXT NULL, original_category_color TEXT NULL, task_created_at TEXT NOT NULL, completed_at TEXT NULL, event_at TEXT NOT NULL, operation_type TEXT NOT NULL, CONSTRAINT fk_history_task FOREIGN KEY (task_id) REFERENCES task(id) ON DELETE CASCADE ON UPDATE RESTRICT);",
};

constexpr const char* kLegacyNote = "老数据第一行\n第二行 with ASCII";

bool BuildV1(const std::filesystem::path& path) {
    EnsureParent(path);
    SqliteConnection connection(path);
    if (!connection.Open().ok()) return false;
    sqlite3* db = connection.Raw();
    for (const char* ddl : kV1Ddl) if (!Exec(db, ddl)) return false;
    if (!Exec(db, "PRAGMA application_id=" + std::to_string(SqliteSchema::kApplicationId) + ";")) return false;
    if (!Exec(db, "PRAGMA user_version=1;")) return false;
    if (!Exec(db, "INSERT INTO schema_migration(version, applied_at) VALUES (1, '2026-01-01 00:00:00');")) return false;
    if (!Exec(db, std::string("INSERT INTO task(id,title,note,priority,created_at,sort_weight,revision) "
                              "VALUES (7,'legacy task','") + kLegacyNote + "','MEDIUM','2026-01-01 00:00:00.000000',1024,1);")) return false;
    return true;
}

}  // namespace

void RunBodyMigrationTests() {
    const auto root = MakeCaseRoot("v2");

    // --- v1 -> v2, idempotent, legacy row untouched ------------------------------
    {
        const auto path = root / "upgrade" / "todolist.db";
        Check("v1 fixture builds", BuildV1(path));
        SqliteUnitOfWorkFactory factory(path);
        auto upgraded = factory.EnsureDatabaseAndSchema();
        Check("v1 -> v2 succeeds", upgraded.ok(), upgraded.ok() ? "" : upgraded.error().message.c_str());
        auto again = factory.EnsureDatabaseAndSchema();
        Check("second upgrade is idempotent", again.ok(), again.ok() ? "" : again.error().message.c_str());
        SqliteConnection connection(path);
        Check("upgraded database reopens", connection.Open().ok());
        sqlite3* db = connection.Raw();
        Check("user_version is 2", Scalar(db, "PRAGMA user_version;") == SqliteSchema::kSchemaVersion);
        Check("schema_migration max is 2", Scalar(db, "SELECT MAX(version) FROM schema_migration;") == 2);
        Check("version 2 row appears once", Scalar(db, "SELECT COUNT(*) FROM schema_migration WHERE version=2;") == 1);
        Check("body_doc column added", Scalar(db, "SELECT COUNT(*) FROM pragma_table_info('task') WHERE name='body_doc';") == 1);
        Check("task_image table created", Scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_image';") == 1);
        Check("task_image index created", Scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='ix_task_image_task';") == 1);
        Check("legacy note byte-for-byte unchanged", Text(db, "SELECT note FROM task WHERE id=7;") == kLegacyNote);
        Check("legacy body_doc stays NULL", Scalar(db, "SELECT COUNT(*) FROM task WHERE id=7 AND body_doc IS NULL;") == 1);
        Check("legacy title unchanged", Text(db, "SELECT title FROM task WHERE id=7;") == "legacy task");

        // Reading the legacy row through the real repository must not write anything back.
        auto uow = factory.Create();
        if (uow.ok()) {
            auto loaded = uow.value()->Tasks().GetByIdForUpdate(7);
            Check("legacy row is readable after upgrade", loaded.ok());
            Check("legacy row exposes empty bodyDoc", loaded.ok() && loaded.value().bodyDoc.empty());
            Check("legacy row keeps its note", loaded.ok() && loaded.value().note == kLegacyNote);
        } else {
            Check("uow opens after upgrade", false);
        }
        Check("still NULL after read-only access", Scalar(db, "SELECT COUNT(*) FROM task WHERE id=7 AND body_doc IS NULL;") == 1);
    }

    // --- Injected V2 failure rolls back entirely, next startup completes ----------
    {
        const auto path = root / "fail" / "todolist.db";
        Check("failure fixture builds", BuildV1(path));
        EnsureParent(path);
        {
            SqliteConnection connection(path);
            Check("failure fixture opens", connection.Open().ok());
            Check("failure trigger installs", Exec(connection.Raw(),
                  "CREATE TRIGGER fail_v2 BEFORE INSERT ON schema_migration WHEN NEW.version=2 "
                  "BEGIN SELECT RAISE(ABORT,'forced v2 failure'); END;"));
        }
        SqliteUnitOfWorkFactory factory(path);
        auto failed = factory.EnsureDatabaseAndSchema();
        Check("injected V2 failure is reported", !failed.ok());
        {
            SqliteConnection connection(path);
            Check("failed database opens", connection.Open().ok());
            sqlite3* db = connection.Raw();
            Check("user_version stays 1 after rollback", Scalar(db, "PRAGMA user_version;") == 1);
            Check("schema_migration stays at 1", Scalar(db, "SELECT MAX(version) FROM schema_migration;") == 1);
            Check("body_doc DDL rolled back", Scalar(db, "SELECT COUNT(*) FROM pragma_table_info('task') WHERE name='body_doc';") == 0);
            Check("task_image DDL rolled back", Scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_image';") == 0);
            Check("legacy data still readable after rollback", Text(db, "SELECT note FROM task WHERE id=7;") == kLegacyNote);
            Check("undoing failure injection", Exec(db, "DROP TRIGGER fail_v2;"));
        }
        auto recovered = factory.EnsureDatabaseAndSchema();
        Check("next startup completes the migration", recovered.ok(), recovered.ok() ? "" : recovered.error().message.c_str());
        SqliteConnection connection(path);
        if (connection.Open().ok()) {
            Check("recovered user_version is 2", Scalar(connection.Raw(), "PRAGMA user_version;") == 2);
            Check("recovered body_doc exists", Scalar(connection.Raw(), "SELECT COUNT(*) FROM pragma_table_info('task') WHERE name='body_doc';") == 1);
        } else {
            Check("recovered database reopens", false);
        }
    }

    // --- Ownership whitelist includes task_image ---------------------------------
    {
        const auto path = root / "adopt" / "todolist.db";
        EnsureParent(path);
        {
            SqliteConnection connection(path);
            Check("adopt fixture opens", connection.Open().ok());
            // application_id stays 0, so HasForeignTables() runs. An interrupted V2 first run can
            // leave exactly this: only task_image exists and the file has not been stamped yet.
            Check("task_image table pre-created", Exec(connection.Raw(),
                  "CREATE TABLE task_image (id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL, "
                  "image_key TEXT NOT NULL COLLATE BINARY, mime TEXT NOT NULL DEFAULT 'image/png', "
                  "width INTEGER NOT NULL, height INTEGER NOT NULL, byte_size INTEGER NOT NULL, "
                  "data BLOB NOT NULL, created_at TEXT NOT NULL);"));
        }
        SqliteUnitOfWorkFactory factory(path);
        auto adopted = factory.EnsureDatabaseAndSchema();
        Check("interrupted V2 database is adopted, not rejected", adopted.ok(),
              adopted.ok() ? "" : adopted.error().message.c_str());
        if (adopted.ok()) {
            SqliteConnection connection(path);
            if (connection.Open().ok()) {
                Check("adopted file gets user_version 2", Scalar(connection.Raw(), "PRAGMA user_version;") == 2);
                Check("adopted file is stamped as ours",
                      Scalar(connection.Raw(), "PRAGMA application_id;") == SqliteSchema::kApplicationId);
            } else {
                Check("adopted file reopens", false);
            }
        }
    }

    // --- A genuinely foreign table is still refused ------------------------------
    {
        const auto path = root / "foreign" / "todolist.db";
        EnsureParent(path);
        {
            SqliteConnection connection(path);
            Check("foreign fixture opens", connection.Open().ok());
            Check("foreign table created", Exec(connection.Raw(), "CREATE TABLE someone_elses_data (x INTEGER);"));
        }
        SqliteUnitOfWorkFactory factory(path);
        auto refused = factory.EnsureDatabaseAndSchema();
        Check("foreign database is refused", !refused.ok() && refused.error().code == ErrorCode::SchemaUnsupported);
    }

    // --- A v3 database is never touched ------------------------------------------
    {
        const auto path = root / "future" / "todolist.db";
        Check("future fixture builds", BuildV1(path));
        EnsureParent(path);
        {
            SqliteConnection connection(path);
            Check("future fixture opens", connection.Open().ok());
            Check("user_version set to 3", Exec(connection.Raw(), "PRAGMA user_version=3;"));
        }
        SqliteUnitOfWorkFactory factory(path);
        auto refused = factory.EnsureDatabaseAndSchema();
        Check("v3 database is refused", !refused.ok() && refused.error().code == ErrorCode::SchemaUnsupported);
        SqliteConnection connection(path);
        if (connection.Open().ok()) {
            Check("v3 note untouched", Text(connection.Raw(), "SELECT note FROM task WHERE id=7;") == kLegacyNote);
            Check("v3 body_doc not added", Scalar(connection.Raw(), "SELECT COUNT(*) FROM pragma_table_info('task') WHERE name='body_doc';") == 0);
        } else {
            Check("v3 database reopens", false);
        }
    }

    Cleanup(root);
}
