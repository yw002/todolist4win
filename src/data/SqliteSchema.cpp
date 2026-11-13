#include "data/SqliteSchema.h"

#include <array>
#include <cstdint>
#include <string>
#include <system_error>

#include <sqlite3.h>

#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"

namespace todo {
namespace {

// Version 1 DDL. Times are fixed-width text "YYYY-MM-DD HH:MM:SS.ffffff" (identical to
// LocalDateTime::ToString()), so lexical order equals chronological order and the day bucket can be
// derived with substr(event_at,1,10) without any timezone conversion.
constexpr const char* kDdl[] = {
    "CREATE TABLE IF NOT EXISTS schema_migration (version INTEGER NOT NULL PRIMARY KEY, applied_at TEXT NOT NULL);",
    "CREATE TABLE IF NOT EXISTS category ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " name TEXT NOT NULL COLLATE NOCASE,"
    " color TEXT NOT NULL,"
    " created_at TEXT NOT NULL,"
    " CONSTRAINT uq_category_name UNIQUE (name));",
    "CREATE TABLE IF NOT EXISTS task ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " category_id INTEGER NULL,"
    " title TEXT NOT NULL,"
    " note TEXT NOT NULL,"
    " priority TEXT NOT NULL,"
    " due_time TEXT NULL,"
    " completed INTEGER NOT NULL DEFAULT 0,"
    " created_at TEXT NOT NULL,"
    " completed_at TEXT NULL,"
    " sort_weight REAL NOT NULL,"
    " revision INTEGER NOT NULL DEFAULT 1,"
    " CONSTRAINT fk_task_category FOREIGN KEY (category_id) REFERENCES category(id) ON DELETE RESTRICT ON UPDATE RESTRICT,"
    " CONSTRAINT ck_task_priority CHECK (priority IN ('HIGH','MEDIUM','LOW')),"
    " CONSTRAINT ck_task_completed CHECK (completed IN (0,1)),"
    " CONSTRAINT ck_task_completion_time CHECK ((completed = 0 AND completed_at IS NULL) OR (completed = 1 AND completed_at IS NOT NULL)));",
    "CREATE TABLE IF NOT EXISTS history ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " task_id INTEGER NOT NULL,"
    " title TEXT NOT NULL,"
    " note TEXT NOT NULL,"
    " priority TEXT NOT NULL,"
    " due_time TEXT NULL,"
    " original_category_id INTEGER NULL,"
    " original_category_name TEXT NULL,"
    " original_category_color TEXT NULL,"
    " task_created_at TEXT NOT NULL,"
    " completed_at TEXT NULL,"
    " event_at TEXT NOT NULL,"
    " operation_type TEXT NOT NULL,"
    " CONSTRAINT fk_history_task FOREIGN KEY (task_id) REFERENCES task(id) ON DELETE CASCADE ON UPDATE RESTRICT,"
    " CONSTRAINT ck_history_priority CHECK (priority IN ('HIGH','MEDIUM','LOW')),"
    " CONSTRAINT ck_history_operation CHECK (operation_type IN ('COMPLETE','UNCOMPLETE')));",
    "CREATE INDEX IF NOT EXISTS ix_task_manual ON task(sort_weight, id);",
    "CREATE INDEX IF NOT EXISTS ix_task_category_manual ON task(category_id, sort_weight, id);",
    "CREATE INDEX IF NOT EXISTS ix_task_due ON task(completed, due_time, sort_weight, id);",
    "CREATE INDEX IF NOT EXISTS ix_task_completed_at ON task(completed, completed_at);",
    "CREATE INDEX IF NOT EXISTS ix_task_category_stats ON task(category_id, completed);",
    "CREATE INDEX IF NOT EXISTS ix_history_event ON history(event_at, id);",
    "CREATE INDEX IF NOT EXISTS ix_history_task ON history(task_id, id);",
    "CREATE INDEX IF NOT EXISTS ix_history_category_event ON history(original_category_id, event_at, id);",
    "CREATE INDEX IF NOT EXISTS ix_history_trend ON history(operation_type, event_at);",
};

Result<std::int64_t> ScalarInt(sqlite3* db, const std::string& sql) {
    SqliteStatement statement(db, sql);
    auto prepared = statement.Prepare();
    if (!prepared.ok()) return Result<std::int64_t>::Failure(prepared.error());
    auto row = statement.Step();
    if (!row.ok()) return Result<std::int64_t>::Failure(row.error());
    if (!row.value()) return Result<std::int64_t>::Failure(InternalSqliteError());
    return Result<std::int64_t>::Success(statement.ColumnInt64(0));
}

// A freshly created (application_id == 0) file is only adopted when it contains no tables outside
// this application's schema. This refuses to take over an unrelated SQLite file that happens to
// live at our path, while still accepting a database left by an interrupted first creation.
Result<bool> HasForeignTables(sqlite3* db) {
    SqliteStatement statement(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';");
    auto prepared = statement.Prepare();
    if (!prepared.ok()) return Result<bool>::Failure(prepared.error());
    static constexpr std::array<const char*, 4> kKnown = {"schema_migration", "category", "task", "history"};
    for (;;) {
        auto row = statement.Step();
        if (!row.ok()) return Result<bool>::Failure(row.error());
        if (!row.value()) break;
        const std::string name = statement.ColumnText(0);
        bool known = false;
        for (const char* candidate : kKnown) {
            if (name == candidate) { known = true; break; }
        }
        if (!known) return Result<bool>::Success(true);
    }
    return Result<bool>::Success(false);
}

}  // namespace

Result<void> SqliteSchema::EnsureDatabaseAndSchema(const std::filesystem::path& path) {
    // 1. Create the parent data directory, mapping ACL/disk failures to an actionable error.
    if (!path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error && !std::filesystem::is_directory(path.parent_path())) {
            return Result<void>::Failure({ErrorCode::PermissionDenied, "无法创建本地数据目录：" + path.parent_path().string()});
        }
    }

    SqliteConnection connection(path);
    auto opened = connection.Open();
    if (!opened.ok()) return opened;
    sqlite3* db = connection.Raw();

    // 2. Ownership check via application_id.
    auto applicationId = ScalarInt(db, "PRAGMA application_id;");
    if (!applicationId.ok()) return Result<void>::Failure(applicationId.error());
    const bool adopt = applicationId.value() == 0;
    if (!adopt && applicationId.value() != kApplicationId) {
        return Result<void>::Failure({ErrorCode::SchemaUnsupported, "该文件不是本应用的本地数据库，已拒绝修改"});
    }
    if (adopt) {
        auto foreign = HasForeignTables(db);
        if (!foreign.ok()) return Result<void>::Failure(foreign.error());
        if (foreign.value()) {
            return Result<void>::Failure({ErrorCode::SchemaUnsupported, "本地数据库包含未知表结构，已拒绝修改"});
        }
    }

    // 3. Version gate up front so a newer database is never touched.
    auto userVersion = ScalarInt(db, "PRAGMA user_version;");
    if (!userVersion.ok()) return Result<void>::Failure(userVersion.error());
    if (userVersion.value() > 1) {
        return Result<void>::Failure({ErrorCode::SchemaUnsupported, "本地数据库架构版本高于当前应用支持版本"});
    }

    if (adopt) {
        auto stamped = connection.Exec("PRAGMA application_id=" + std::to_string(kApplicationId) + ";");
        if (!stamped.ok()) return stamped;
    }

    // 4. Create schema inside a write transaction so a partial DDL run is rolled back.
    auto begun = connection.Exec("BEGIN IMMEDIATE;");
    if (!begun.ok()) return begun;
    auto rollback = [&connection]() {
        // Best effort: the caller is already returning a failure, so a rollback error
        // would only mask the original cause.
        const auto rolled = connection.Exec("ROLLBACK;");
        (void)rolled;
    };

    for (const char* ddl : kDdl) {
        auto applied = connection.Exec(ddl);
        if (!applied.ok()) { rollback(); return applied; }
    }

    auto maxVersion = ScalarInt(db, "SELECT COALESCE(MAX(version),0) FROM schema_migration;");
    if (!maxVersion.ok()) { rollback(); return Result<void>::Failure(maxVersion.error()); }
    if (maxVersion.value() > 1) {
        rollback();
        return Result<void>::Failure({ErrorCode::SchemaUnsupported, "本地数据库架构版本高于当前应用支持版本"});
    }
    if (maxVersion.value() == 0) {
        // applied_at is diagnostic only; datetime() avoids a SystemClock dependency in the data
        // layer and keeps the value local-time, matching the rest of the schema.
        auto inserted = connection.Exec("INSERT INTO schema_migration(version, applied_at) VALUES (1, datetime('now','localtime'));");
        if (!inserted.ok()) { rollback(); return inserted; }
    }

    auto committed = connection.Exec("COMMIT;");
    if (!committed.ok()) { rollback(); return committed; }

    // 5. Persist the user_version for external diagnostics as well; harmless if already 1.
    auto versioned = connection.Exec("PRAGMA user_version=1;");
    if (!versioned.ok()) return versioned;
    return Result<void>::Success();
}

}  // namespace todo
