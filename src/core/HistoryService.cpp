#include "core/HistoryService.h"

#include "core/TaskService.h"
#include "domain/TaskRules.h"

#include <memory>
#include <string>
#include <vector>

namespace todo {

HistoryService::HistoryService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock) : factory_(factory), clock_(std::move(clock)) {}

Result<Task> HistoryService::Restore(std::int64_t taskId) {
    auto valid = TaskRules::ValidateId(taskId);
    if (!valid.ok()) return Result<Task>::Failure(valid.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Task>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Task>::Failure(begun.error());
    TransactionGuard guard(uow);
    auto locked = uow.Tasks().GetByIdForUpdate(taskId);
    if (!locked.ok()) return locked;
    Task task = locked.value();
    if (!task.completed) return Result<Task>::Failure({ErrorCode::Conflict, "任务当前不是已完成状态"});
    const LocalDateTime now = clock_->NowLocal();
    const auto revision = task.revision;
    task.completed = false;
    task.completedAt.reset();
    auto updated = uow.Tasks().Update(task, revision);
    if (!updated.ok()) return updated;
    auto event = TaskService::MakeHistoryEvent(uow, updated.value(), OperationType::Uncomplete, now);
    if (!event.ok()) return Result<Task>::Failure(event.error());
    auto appended = uow.History().Append(event.value());
    if (!appended.ok()) return Result<Task>::Failure(appended.error());
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Task>::Failure(committed.error());
    guard.Committed();
    return updated;
}

BatchResult HistoryService::RestoreBatch(const std::vector<std::int64_t>& ids) {
    BatchResult batch;
    for (const std::int64_t id : ids) {
        auto result = Restore(id);
        batch.items.push_back({id, result.ok(), result.ok() ? std::string{} : result.error().message});
    }
    return batch;
}

Result<void> HistoryService::DeleteEvents(const std::vector<std::int64_t>& ids) {
    if (ids.empty() || ids.size() > 200) return Result<void>::Failure({ErrorCode::InvalidInput, "请选择 1 到 200 条历史记录"});
    for (auto id : ids) { auto valid = TaskRules::ValidateId(id); if (!valid.ok()) return valid; }
    auto created = factory_.Create();
    if (!created.ok()) return Result<void>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return begun;
    TransactionGuard guard(uow);
    auto result = uow.History().DeleteEvents(ids);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return committed;
    guard.Committed();
    return Result<void>::Success();
}

Result<HistoryPage> HistoryService::Query(const HistoryQuery& query) {
    auto keyword = TaskRules::ValidateKeyword(query.filter.keyword);
    if (!keyword.ok()) return Result<HistoryPage>::Failure(keyword.error());
    if (query.page.pageSize <= 0 || query.page.pageSize > 200) return Result<HistoryPage>::Failure({ErrorCode::InvalidInput, "分页大小必须在 1 到 200 之间"});
    auto created = factory_.Create();
    if (!created.ok()) return Result<HistoryPage>::Failure(created.error());
    return created.value()->History().QueryPage(query);
}

}  // namespace todo
