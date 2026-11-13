#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <sqlite3.h>

#include "common/Result.h"

namespace todo {

// Owns a single sqlite3* for one unit of work. A connection is never shared across threads: the
// application has exactly one DbWorker thread, and SQLITE_OPEN_FULLMUTEX is only a defensive extra
// against an accidental cross-thread call. Open() applies the connection PRAGMAs from the design
// (foreign_keys, busy_timeout, synchronous) and enables WAL on the database.
class SqliteConnection {
public:
    explicit SqliteConnection(std::filesystem::path path);
    ~SqliteConnection();
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    [[nodiscard]] Result<void> Open();
    [[nodiscard]] sqlite3* Raw() noexcept { return db_; }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    [[nodiscard]] Result<void> Exec(const std::string& sql);
    [[nodiscard]] std::int64_t LastInsertRowId() const noexcept;
    [[nodiscard]] int Changes() const noexcept;

private:
    std::filesystem::path path_;
    sqlite3* db_{nullptr};
};

// RAII prepared statement with typed bind/column helpers. Prepare() must be called and checked
// before use. Step() distinguishes SQLITE_ROW (true) from SQLITE_DONE (false) so callers can write
// the same row-loop shape as the MySQL repositories.
class SqliteStatement {
public:
    SqliteStatement(sqlite3* db, std::string sql);
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    [[nodiscard]] Result<void> Prepare();
    [[nodiscard]] sqlite3_stmt* Raw() noexcept { return stmt_; }

    void BindInt64(int index, std::int64_t value);
    void BindDouble(int index, double value);
    void BindText(int index, const std::string& value);
    void BindNull(int index);

    [[nodiscard]] Result<bool> Step();  // true = a row is available, false = statement finished
    [[nodiscard]] Result<void> Run();   // step to completion for statements that return no rows

    [[nodiscard]] std::int64_t ColumnInt64(int index) const;
    [[nodiscard]] double ColumnDouble(int index) const;
    [[nodiscard]] std::string ColumnText(int index) const;
    [[nodiscard]] bool ColumnIsNull(int index) const;

    void Reset();

private:
    sqlite3* db_{nullptr};
    sqlite3_stmt* stmt_{nullptr};
    std::string sql_;
};

}  // namespace todo
