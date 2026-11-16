#include "data/SqliteConnection.h"

#include <utility>

#include "data/SqliteErrors.h"

namespace todo {
namespace {

// SQLite interprets open_v2 filenames as UTF-8, so the path must not be narrowed through the
// process ANSI code page (which would corrupt non-ASCII %LOCALAPPDATA% paths).
std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.c_str()), encoded.size());
}

}  // namespace

SqliteConnection::SqliteConnection(std::filesystem::path path) : path_(std::move(path)) {}
SqliteConnection::~SqliteConnection() {
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Result<void> SqliteConnection::Open() {
    if (db_ != nullptr) return Result<void>::Success();
    const std::string name = ToUtf8(path_);
    const int rc = sqlite3_open_v2(name.c_str(), &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        AppError error = TranslateSqliteResult(rc);
        error.message = "本地数据库无法打开：" + name;
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        return Result<void>::Failure(error);
    }
    // Extended result codes let the repositories distinguish UNIQUE / FOREIGNKEY / CHECK failures.
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, 5000);
    for (const char* pragma : {"PRAGMA foreign_keys=ON;", "PRAGMA synchronous=NORMAL;", "PRAGMA busy_timeout=5000;"}) {
        auto applied = Exec(pragma);
        if (!applied.ok()) return applied;
    }
    // journal_mode is a database-level, persistent setting; it cannot be changed inside a
    // transaction. Best-effort: a database on a filesystem that cannot use WAL still works with
    // the default rollback journal, so a non-"wal" answer is not treated as a hard failure.
    auto wal = Exec("PRAGMA journal_mode=WAL;");
    if (!wal.ok()) return wal;
    return Result<void>::Success();
}

Result<void> SqliteConnection::Exec(const std::string& sql) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    char* errorText = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errorText);
    if (errorText != nullptr) sqlite3_free(errorText);
    if (rc != SQLITE_OK) return Result<void>::Failure(TranslateSqliteResult(rc));
    return Result<void>::Success();
}

std::int64_t SqliteConnection::LastInsertRowId() const noexcept {
    return db_ != nullptr ? sqlite3_last_insert_rowid(db_) : 0;
}

int SqliteConnection::Changes() const noexcept { return db_ != nullptr ? sqlite3_changes(db_) : 0; }

SqliteStatement::SqliteStatement(sqlite3* db, std::string sql) : db_(db), sql_(std::move(sql)) {}
SqliteStatement::~SqliteStatement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

Result<void> SqliteStatement::Prepare() {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    if (stmt_ != nullptr) return Result<void>::Success();
    const int rc = sqlite3_prepare_v2(db_, sql_.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        stmt_ = nullptr;
        return Result<void>::Failure(TranslateSqliteResult(rc));
    }
    return Result<void>::Success();
}

void SqliteStatement::BindInt64(int index, std::int64_t value) {
    if (stmt_ != nullptr) sqlite3_bind_int64(stmt_, index, value);
}
void SqliteStatement::BindDouble(int index, double value) {
    if (stmt_ != nullptr) sqlite3_bind_double(stmt_, index, value);
}
void SqliteStatement::BindText(int index, const std::string& value) {
    if (stmt_ != nullptr) {
        sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
}
void SqliteStatement::BindNull(int index) {
    if (stmt_ != nullptr) sqlite3_bind_null(stmt_, index);
}
void SqliteStatement::BindBlob(int index, const std::vector<std::byte>& value) {
    if (stmt_ == nullptr) return;
    // A null pointer with length 0 is documented as a zero-length BLOB, which is why an explicit
    // non-null placeholder is used for the empty case.
    static constexpr std::byte kEmpty{0};
    const void* data = value.empty() ? static_cast<const void*>(&kEmpty) : static_cast<const void*>(value.data());
    sqlite3_bind_blob(stmt_, index, data, static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

Result<bool> SqliteStatement::Step() {
    if (stmt_ == nullptr) return Result<bool>::Failure(InternalSqliteError());
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return Result<bool>::Success(true);
    if (rc == SQLITE_DONE) return Result<bool>::Success(false);
    return Result<bool>::Failure(TranslateSqliteResult(rc));
}

Result<void> SqliteStatement::Run() {
    for (;;) {
        auto result = Step();
        if (!result.ok()) return Result<void>::Failure(result.error());
        if (!result.value()) return Result<void>::Success();
    }
}

std::int64_t SqliteStatement::ColumnInt64(int index) const {
    return stmt_ != nullptr ? sqlite3_column_int64(stmt_, index) : 0;
}
double SqliteStatement::ColumnDouble(int index) const {
    return stmt_ != nullptr ? sqlite3_column_double(stmt_, index) : 0.0;
}
std::string SqliteStatement::ColumnText(int index) const {
    if (stmt_ == nullptr) return {};
    const auto* text = sqlite3_column_text(stmt_, index);
    if (text == nullptr) return {};
    const int bytes = sqlite3_column_bytes(stmt_, index);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}
bool SqliteStatement::ColumnIsNull(int index) const {
    return stmt_ == nullptr || sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}
std::vector<std::byte> SqliteStatement::ColumnBlob(int index) const {
    if (stmt_ == nullptr) return {};
    const void* data = sqlite3_column_blob(stmt_, index);
    const int bytes = sqlite3_column_bytes(stmt_, index);
    if (data == nullptr || bytes <= 0) return {};
    const auto* first = static_cast<const unsigned char*>(data);
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(bytes));
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::byte>(first[i]));
    return out;
}
void SqliteStatement::Reset() {
    if (stmt_ != nullptr) sqlite3_reset(stmt_);
}

}  // namespace todo
