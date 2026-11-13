#pragma once

#include <memory>
#include <mutex>

#include "config/AppConfig.h"
#include "core/RepositoryInterfaces.h"
#include "data/MySqlCategoryRepository.h"
#include "data/MySqlConnection.h"
#include "data/MySqlHistoryRepository.h"
#include "data/MySqlStatisticsRepository.h"
#include "data/MySqlTaskRepository.h"

namespace todo {

class MySqlUnitOfWork final : public IUnitOfWork {
public:
    explicit MySqlUnitOfWork(std::unique_ptr<MySqlConnection> connection);
    ~MySqlUnitOfWork() override;
    Result<void> Begin() override;
    Result<void> Commit() override;
    void Rollback() noexcept override;
    ITaskRepository& Tasks() override;
    ICategoryRepository& Categories() override;
    IHistoryRepository& History() override;
    IStatisticsRepository& Statistics() override;

private:
    std::unique_ptr<MySqlConnection> connection_;
    MySqlTaskRepository tasks_;
    MySqlCategoryRepository categories_;
    MySqlHistoryRepository history_;
    MySqlStatisticsRepository statistics_;
    bool active_{false};
};

class MySqlUnitOfWorkFactory final : public IUnitOfWorkFactory {
public:
    explicit MySqlUnitOfWorkFactory(DatabaseConfig config);
    Result<std::unique_ptr<IUnitOfWork>> Create() override;
    Result<void> EnsureDatabaseAndSchema() override;
    // No unlocked Config() accessor: the worker thread reads the config through Create() and
    // EnsureDatabaseAndSchema(), both of which copy it under configMutex_.
    void UpdateConfig(const DatabaseConfig& config) noexcept;

private:
    mutable std::mutex configMutex_;
    DatabaseConfig config_;
};

}  // namespace todo
