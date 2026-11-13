#include "data/MySqlUnitOfWork.h"

#include <jdbc/cppconn/connection.h>
#include <memory>
#include <mutex>

#include "data/MySqlErrors.h"
#include "data/MySqlSchema.h"

namespace todo {

MySqlUnitOfWork::MySqlUnitOfWork(std::unique_ptr<MySqlConnection> connection)
    : connection_(std::move(connection)), tasks_(&connection_->Get()), categories_(&connection_->Get()), history_(&connection_->Get()), statistics_(&connection_->Get()) {}
MySqlUnitOfWork::~MySqlUnitOfWork() { if (active_) Rollback(); }

Result<void> MySqlUnitOfWork::Begin() {
    try {
        connection_->Get().setAutoCommit(false);
        connection_->Get().setTransactionIsolation(sql::TRANSACTION_REPEATABLE_READ);
        active_ = true;
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlUnitOfWork::Commit() {
    if (!active_) return Result<void>::Failure({ErrorCode::Internal, "事务尚未开始"});
    try {
        connection_->Get().commit();
        connection_->Get().setAutoCommit(true);
        active_ = false;
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

void MySqlUnitOfWork::Rollback() noexcept {
    if (!active_) return;
    try { connection_->Get().rollback(); connection_->Get().setAutoCommit(true); } catch (...) {}
    active_ = false;
}

ITaskRepository& MySqlUnitOfWork::Tasks() { return tasks_; }
ICategoryRepository& MySqlUnitOfWork::Categories() { return categories_; }
IHistoryRepository& MySqlUnitOfWork::History() { return history_; }
IStatisticsRepository& MySqlUnitOfWork::Statistics() { return statistics_; }

MySqlUnitOfWorkFactory::MySqlUnitOfWorkFactory(DatabaseConfig config) : config_(std::move(config)) {}

Result<std::unique_ptr<IUnitOfWork>> MySqlUnitOfWorkFactory::Create() {
    DatabaseConfig config;
    { std::lock_guard lock(configMutex_); config = config_; }
    auto connection = std::make_unique<MySqlConnection>(std::move(config));
    auto connected = connection->ConnectDatabase();
    if (!connected.ok()) return Result<std::unique_ptr<IUnitOfWork>>::Failure(connected.error());
    return Result<std::unique_ptr<IUnitOfWork>>::Success(std::make_unique<MySqlUnitOfWork>(std::move(connection)));
}

Result<void> MySqlUnitOfWorkFactory::EnsureDatabaseAndSchema() { DatabaseConfig config; { std::lock_guard lock(configMutex_); config = config_; } return MySqlSchema::EnsureDatabaseAndSchema(config); }
void MySqlUnitOfWorkFactory::UpdateConfig(const DatabaseConfig& config) noexcept { std::lock_guard lock(configMutex_); config_ = config; }

}  // namespace todo
