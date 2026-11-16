#include "data/SqliteUnitOfWork.h"

#include <utility>

#include "data/SqliteErrors.h"
#include "data/SqliteSchema.h"

namespace todo {

SqliteUnitOfWork::SqliteUnitOfWork(std::unique_ptr<SqliteConnection> connection)
    : connection_(std::move(connection)),
      tasks_(connection_->Raw()),
      categories_(connection_->Raw()),
      history_(connection_->Raw()),
      statistics_(connection_->Raw()),
      images_(connection_->Raw()) {}

SqliteUnitOfWork::~SqliteUnitOfWork() {
    if (active_) Rollback();
}

Result<void> SqliteUnitOfWork::Begin() {
    if (active_) return Result<void>::Failure({ErrorCode::Internal, "事务已经开始"});
    auto begun = connection_->Exec("BEGIN IMMEDIATE;");
    if (!begun.ok()) return begun;
    active_ = true;
    return Result<void>::Success();
}

Result<void> SqliteUnitOfWork::Commit() {
    if (!active_) return Result<void>::Failure({ErrorCode::Internal, "事务尚未开始"});
    auto committed = connection_->Exec("COMMIT;");
    if (!committed.ok()) return committed;
    active_ = false;
    return Result<void>::Success();
}

void SqliteUnitOfWork::Rollback() noexcept {
    if (!active_) return;
    // Rollback is best-effort inside a noexcept destructor path; the error carries
    // nothing actionable because the transaction is being abandoned anyway.
    const auto rolled = connection_->Exec("ROLLBACK;");
    (void)rolled;
    active_ = false;
}

ITaskRepository& SqliteUnitOfWork::Tasks() { return tasks_; }
ICategoryRepository& SqliteUnitOfWork::Categories() { return categories_; }
IHistoryRepository& SqliteUnitOfWork::History() { return history_; }
IStatisticsRepository& SqliteUnitOfWork::Statistics() { return statistics_; }
ITaskImageRepository& SqliteUnitOfWork::Images() { return images_; }

SqliteUnitOfWorkFactory::SqliteUnitOfWorkFactory(std::filesystem::path path) : path_(std::move(path)) {}

Result<std::unique_ptr<IUnitOfWork>> SqliteUnitOfWorkFactory::Create() {
    auto connection = std::make_unique<SqliteConnection>(path_);
    auto opened = connection->Open();
    if (!opened.ok()) return Result<std::unique_ptr<IUnitOfWork>>::Failure(opened.error());
    return Result<std::unique_ptr<IUnitOfWork>>::Success(std::make_unique<SqliteUnitOfWork>(std::move(connection)));
}

Result<void> SqliteUnitOfWorkFactory::EnsureDatabaseAndSchema() { return SqliteSchema::EnsureDatabaseAndSchema(path_); }

const std::filesystem::path& SqliteUnitOfWorkFactory::Path() const noexcept { return path_; }

}  // namespace todo
