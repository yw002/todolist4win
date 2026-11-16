// SQLite-specific repository tests: schema idempotency, ownership/version guards, and the shared
// service-level contract run against a real temporary database file. The temporary directory is
// created under %TEMP% and removed afterwards; the real %LOCALAPPDATA%\TodoListWin is never touched.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <windows.h>

#include "common/Error.h"
#include "data/SqliteConnection.h"
#include "data/SqliteSchema.h"
#include "data/SqliteUnitOfWork.h"

void ReportTest(const char*, const char*, bool, const char*);
void RunRepositoryContract(const char*, todo::IUnitOfWorkFactory&);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") { ReportTest("SqliteRepository", name, value, detail); }

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_sqlite_") + tag + "_" +
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

std::int64_t PragmaInt(sqlite3* db, const char* sql, bool& ok) {
    SqliteStatement statement(db, sql);
    auto prepared = statement.Prepare();
    if (!prepared.ok()) { ok = false; return -1; }
    auto row = statement.Step();
    if (!row.ok() || !row.value()) { ok = false; return -1; }
    ok = true;
    return statement.ColumnInt64(0);
}

}  // namespace

void RunSqliteRepositoryTests() {
    const auto root = MakeCaseRoot("repo");
    const auto databasePath = root / "data" / "todolist.db";

    // --- Schema creation is idempotent ----------------------------------------------
    SqliteUnitOfWorkFactory factory(databasePath);
    auto firstEnsure = factory.EnsureDatabaseAndSchema();
    Check("first schema ensure succeeds", firstEnsure.ok(), firstEnsure.ok() ? "" : firstEnsure.error().message.c_str());
    auto secondEnsure = factory.EnsureDatabaseAndSchema();
    Check("second schema ensure is idempotent", secondEnsure.ok(), secondEnsure.ok() ? "" : secondEnsure.error().message.c_str());

    // --- application_id / user_version are stamped -----------------------------------
    {
        SqliteConnection connection(databasePath);
        auto opened = connection.Open();
        Check("database reopens", opened.ok());
        if (opened.ok()) {
            bool ok = false;
            const std::int64_t applicationId = PragmaInt(connection.Raw(), "PRAGMA application_id;", ok);
            Check("application_id is the TDLW magic", ok && applicationId == SqliteSchema::kApplicationId);
            bool versionOk = false;
            const std::int64_t userVersion = PragmaInt(connection.Raw(), "PRAGMA user_version;", versionOk);
            Check("user_version is 2", versionOk && userVersion == SqliteSchema::kSchemaVersion);
        }
    }

    // --- Shared service-level contract ----------------------------------------------
    RunRepositoryContract("SqliteContract", factory);

    // --- A foreign database at our path is rejected, not modified --------------------
    {
        const auto foreignRoot = MakeCaseRoot("foreign");
        const auto foreignPath = foreignRoot / "data" / "foreign.db";
        std::error_code error;
        std::filesystem::create_directories(foreignPath.parent_path(), error);
        {
            SqliteConnection connection(foreignPath);
            auto opened = connection.Open();
            Check("foreign database opens", opened.ok());
            if (opened.ok()) {
                auto seeded = connection.Exec("CREATE TABLE unrelated(value TEXT); PRAGMA application_id=12345;");
                Check("foreign database seeded", seeded.ok(), seeded.ok() ? "" : seeded.error().message.c_str());
            }
        }
        SqliteUnitOfWorkFactory foreignFactory(foreignPath);
        auto rejected = foreignFactory.EnsureDatabaseAndSchema();
        Check("foreign application_id is SchemaUnsupported",
              !rejected.ok() && rejected.error().code == ErrorCode::SchemaUnsupported);
        Cleanup(foreignRoot);
    }

    // --- A database claiming a newer schema version is rejected ----------------------
    {
        const auto futureRoot = MakeCaseRoot("future");
        const auto futurePath = futureRoot / "data" / "future.db";
        std::error_code error;
        std::filesystem::create_directories(futurePath.parent_path(), error);
        {
            SqliteConnection connection(futurePath);
            if (connection.Open().ok()) {
                connection.Exec("PRAGMA application_id=" + std::to_string(SqliteSchema::kApplicationId) + "; PRAGMA user_version=3;");
            }
        }
        SqliteUnitOfWorkFactory futureFactory(futurePath);
        auto rejected = futureFactory.EnsureDatabaseAndSchema();
        Check("newer user_version is SchemaUnsupported",
              !rejected.ok() && rejected.error().code == ErrorCode::SchemaUnsupported);
        Cleanup(futureRoot);
    }

    // --- Create() opens a working unit of work --------------------------------------
    {
        auto uow = factory.Create();
        Check("factory creates a unit of work", uow.ok(), uow.ok() ? "" : uow.error().message.c_str());
        if (uow.ok()) {
            auto begun = uow.value()->Begin();
            Check("unit of work begins", begun.ok(), begun.ok() ? "" : begun.error().message.c_str());
            uow.value()->Rollback();
        }
    }

    Cleanup(root);
}
