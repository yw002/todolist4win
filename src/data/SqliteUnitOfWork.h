#pragma once

#include <filesystem>
#include <memory>

#include "core/RepositoryInterfaces.h"
#include "data/SqliteCategoryRepository.h"
#include "data/SqliteConnection.h"
#include "data/SqliteHistoryRepository.h"
#include "data/SqliteStatisticsRepository.h"
#include "data/SqliteTaskImageRepository.h"
#include "data/SqliteTaskRepository.h"

namespace todo {

// One unit of work == one sqlite3 connection. Begin() issues BEGIN IMMEDIATE so a write takes the
// write lock up front (no read-then-upgrade deadlock). Rollback runs on destruction when a caller
// forgets to commit. This follows the unit-of-work contract exactly.
class SqliteUnitOfWork final : public IUnitOfWork {
public:
    explicit SqliteUnitOfWork(std::unique_ptr<SqliteConnection> connection);
    ~SqliteUnitOfWork() override;
    Result<void> Begin() override;
    Result<void> Commit() override;
    void Rollback() noexcept override;
    ITaskRepository& Tasks() override;
    ICategoryRepository& Categories() override;
    IHistoryRepository& History() override;
    IStatisticsRepository& Statistics() override;
    ITaskImageRepository& Images() override;

private:
    std::unique_ptr<SqliteConnection> connection_;
    SqliteTaskRepository tasks_;
    SqliteCategoryRepository categories_;
    SqliteHistoryRepository history_;
    SqliteStatisticsRepository statistics_;
    SqliteTaskImageRepository images_;
    bool active_{false};
};

// Factory for the local database. Create() opens a fresh connection per unit of work; no sqlite3*
// is ever shared across units. EnsureDatabaseAndSchema() delegates to SqliteSchema.
class SqliteUnitOfWorkFactory final : public IUnitOfWorkFactory {
public:
    explicit SqliteUnitOfWorkFactory(std::filesystem::path path);
    Result<std::unique_ptr<IUnitOfWork>> Create() override;
    Result<void> EnsureDatabaseAndSchema() override;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

private:
    std::filesystem::path path_;
};

}  // namespace todo
