#include "data/MySqlSchema.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>

#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>

#include "common/Clock.h"
#include "data/MySqlErrors.h"

namespace todo {
namespace {
bool ValidDatabaseName(const std::string& name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    });
}

Result<void> Execute(sql::Connection& connection, const std::string& sqlText) {
    try {
        std::unique_ptr<sql::Statement> statement(connection.createStatement());
        statement->execute(sqlText);
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}
}  // namespace

Result<void> MySqlSchema::EnsureDatabaseAndSchema(const DatabaseConfig& config) {
    if (!ValidDatabaseName(config.database)) return Result<void>::Failure({ErrorCode::InvalidInput, "数据库名只能包含字母、数字和下划线"});
    MySqlConnection server(config);
    auto connected = server.ConnectServer();
    if (!connected.ok()) return connected;
    try {
        std::unique_ptr<sql::Statement> statement(server.Get().createStatement());
        std::unique_ptr<sql::ResultSet> version(statement->executeQuery("SELECT VERSION()"));
        if (!version->next()) return Result<void>::Failure({ErrorCode::SchemaUnsupported, "无法识别数据库版本"});
        const std::string text = version->getString(1).c_str();
        int major = 0, minor = 0, patch = 0;
        if (std::sscanf(text.c_str(), "%d.%d.%d", &major, &minor, &patch) != 3 || major < 8 || (major == 8 && minor == 0 && patch < 16)) {
            return Result<void>::Failure({ErrorCode::SchemaUnsupported, "需要 MySQL 8.0.16 或更新版本"});
        }
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
    auto database = Execute(server.Get(), "CREATE DATABASE IF NOT EXISTS `" + config.database + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci");
    if (!database.ok()) return database;
    MySqlConnection business(config);
    connected = business.ConnectDatabase();
    if (!connected.ok()) return connected;
    return EnsureTables(business.Get());
}

Result<void> MySqlSchema::EnsureTables(sql::Connection& connection) {
    auto migrationTable = Execute(connection, "CREATE TABLE IF NOT EXISTS schema_migration (version INT NOT NULL PRIMARY KEY, applied_at DATETIME(6) NOT NULL) ENGINE=InnoDB");
    if (!migrationTable.ok()) return migrationTable;
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection.prepareStatement("SELECT MAX(version) FROM schema_migration"));
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        if (rows->next() && !rows->isNull(1) && rows->getInt(1) > 1) return Result<void>::Failure({ErrorCode::SchemaUnsupported, "数据库架构版本高于当前应用支持版本"});
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
    static constexpr const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS category (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, name VARCHAR(80) NOT NULL, color CHAR(7) NOT NULL, created_at DATETIME(6) NOT NULL, CONSTRAINT uq_category_name UNIQUE (name)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS task (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, category_id BIGINT UNSIGNED NULL, title VARCHAR(500) NOT NULL, note TEXT NOT NULL, priority VARCHAR(8) NOT NULL, due_time DATETIME(6) NULL, completed TINYINT(1) NOT NULL DEFAULT 0, created_at DATETIME(6) NOT NULL, completed_at DATETIME(6) NULL, sort_weight DOUBLE NOT NULL, revision BIGINT UNSIGNED NOT NULL DEFAULT 1, CONSTRAINT fk_task_category FOREIGN KEY (category_id) REFERENCES category(id) ON DELETE RESTRICT ON UPDATE RESTRICT, CONSTRAINT ck_task_priority CHECK (priority IN ('HIGH','MEDIUM','LOW')), CONSTRAINT ck_task_completed CHECK (completed IN (0,1)), CONSTRAINT ck_task_completion_time CHECK ((completed = 0 AND completed_at IS NULL) OR (completed = 1 AND completed_at IS NOT NULL)), INDEX ix_task_manual (sort_weight, id), INDEX ix_task_category_manual (category_id, sort_weight, id), INDEX ix_task_due (completed, due_time, sort_weight, id), INDEX ix_task_completed_at (completed, completed_at), INDEX ix_task_category_stats (category_id, completed)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS history (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, task_id BIGINT UNSIGNED NOT NULL, title VARCHAR(500) NOT NULL, note TEXT NOT NULL, priority VARCHAR(8) NOT NULL, due_time DATETIME(6) NULL, original_category_id BIGINT UNSIGNED NULL, original_category_name VARCHAR(80) NULL, original_category_color CHAR(7) NULL, task_created_at DATETIME(6) NOT NULL, completed_at DATETIME(6) NULL, event_at DATETIME(6) NOT NULL, operation_type VARCHAR(16) NOT NULL, CONSTRAINT fk_history_task FOREIGN KEY (task_id) REFERENCES task(id) ON DELETE CASCADE ON UPDATE RESTRICT, CONSTRAINT ck_history_priority CHECK (priority IN ('HIGH','MEDIUM','LOW')), CONSTRAINT ck_history_operation CHECK (operation_type IN ('COMPLETE','UNCOMPLETE')), INDEX ix_history_event (event_at, id), INDEX ix_history_task (task_id, id), INDEX ix_history_category_event (original_category_id, event_at, id), INDEX ix_history_trend (operation_type, event_at)) ENGINE=InnoDB"
    };
    for (const char* text : ddl) { auto result = Execute(connection, text); if (!result.ok()) return result; }
    try {
        std::unique_ptr<sql::PreparedStatement> insert(connection.prepareStatement("INSERT IGNORE INTO schema_migration(version, applied_at) VALUES (1, ?)"));
        insert->setString(1, SystemClock{}.NowLocal().ToString());
        insert->executeUpdate();
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

}  // namespace todo
