#include "core/TaskService.h"

#include <memory>
#include <optional>
#include <utility>

#include "domain/TaskRules.h"

namespace todo {
namespace {
template <typename T>
Result<T> FactoryError(Result<std::unique_ptr<IUnitOfWork>> result) {
    return Result<T>::Failure(result.error());
}
Result<void> ValidateCategoryId(std::optional<std::int64_t> id) {
    return id ? TaskRules::ValidateId(*id) : Result<void>::Success();
}
}  // namespace

TaskService::TaskService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock) : factory_(factory), clock_(std::move(clock)) {}

Result<Task> TaskService::Create(const CreateTaskCommand& command) {
    auto title = TaskRules::ValidateTitle(command.title);
    if (!title.ok()) return Result<Task>::Failure(title.error());
    auto note = TaskRules::ValidateNote(command.note);
    if (!note.ok()) return Result<Task>::Failure(note.error());
    auto category = ValidateCategoryId(command.categoryId);
    if (!category.ok()) return Result<Task>::Failure(category.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Task>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Task>::Failure(begun.error());
    TransactionGuard guard(uow);
    Task task;
    task.title = TaskRules::Trim(command.title);
    task.note = command.note;
    task.priority = command.priority;
    task.dueAt = command.dueAt;
    task.categoryId = command.categoryId;
    task.createdAt = clock_->NowLocal();
    auto result = uow.Tasks().Create(task);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Task>::Failure(committed.error());
    guard.Committed();
    return result;
}

Result<Task> TaskService::Edit(const EditTaskCommand& command) {
    auto id = TaskRules::ValidateId(command.id);
    if (!id.ok()) return Result<Task>::Failure(id.error());
    auto title = TaskRules::ValidateTitle(command.title);
    if (!title.ok()) return Result<Task>::Failure(title.error());
    auto note = TaskRules::ValidateNote(command.note);
    if (!note.ok()) return Result<Task>::Failure(note.error());
    auto category = ValidateCategoryId(command.categoryId);
    if (!category.ok()) return Result<Task>::Failure(category.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Task>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Task>::Failure(begun.error());
    TransactionGuard guard(uow);
    auto current = uow.Tasks().GetByIdForUpdate(command.id);
    if (!current.ok()) return current;
    if (current.value().revision != command.expectedRevision) return Result<Task>::Failure({ErrorCode::Conflict, "任务已被修改，请刷新后重试"});
    Task updated = current.value();
    updated.title = TaskRules::Trim(command.title);
    updated.note = command.note;
    updated.priority = command.priority;
    updated.dueAt = command.dueAt;
    updated.categoryId = command.categoryId;
    auto result = uow.Tasks().Update(updated, command.expectedRevision);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Task>::Failure(committed.error());
    guard.Committed();
    return result;
}

Result<Task> TaskService::SetCompleted(std::int64_t id, bool value, std::uint64_t expectedRevision) {
    auto valid = TaskRules::ValidateId(id);
    if (!valid.ok()) return Result<Task>::Failure(valid.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Task>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Task>::Failure(begun.error());
    TransactionGuard guard(uow);
    auto locked = uow.Tasks().GetByIdForUpdate(id);
    if (!locked.ok()) return locked;
    Task task = locked.value();
    if (task.revision != expectedRevision) return Result<Task>::Failure({ErrorCode::Conflict, "任务状态已变化，请刷新后重试"});
    if (task.completed == value) return Result<Task>::Success(task);
    const LocalDateTime now = clock_->NowLocal();
    task.completed = value;
    task.completedAt = value ? std::optional<LocalDateTime>(now) : std::nullopt;
    auto updated = uow.Tasks().Update(task, expectedRevision);
    if (!updated.ok()) return updated;
    auto event = MakeHistoryEvent(uow, updated.value(), value ? OperationType::Complete : OperationType::Uncomplete, now);
    if (!event.ok()) return Result<Task>::Failure(event.error());
    auto appended = uow.History().Append(event.value());
    if (!appended.ok()) return Result<Task>::Failure(appended.error());
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Task>::Failure(committed.error());
    guard.Committed();
    return updated;
}

Result<void> TaskService::Delete(std::int64_t id) {
    auto valid = TaskRules::ValidateId(id);
    if (!valid.ok()) return valid;
    auto created = factory_.Create();
    if (!created.ok()) return Result<void>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return begun;
    TransactionGuard guard(uow);
    auto result = uow.Tasks().DeleteTask(id);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return committed;
    guard.Committed();
    return Result<void>::Success();
}

Result<void> TaskService::Reorder(const ReorderRequest& command) {
    auto valid = TaskRules::ValidateId(command.taskId);
    if (!valid.ok()) return valid;
    auto created = factory_.Create();
    if (!created.ok()) return Result<void>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return begun;
    TransactionGuard guard(uow);
    auto result = uow.Tasks().Reorder(command);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return committed;
    guard.Committed();
    return Result<void>::Success();
}

Result<TaskPage> TaskService::Query(const TaskQuery& query) {
    auto keyword = TaskRules::ValidateKeyword(query.filter.keyword);
    if (!keyword.ok()) return Result<TaskPage>::Failure(keyword.error());
    if (query.page.pageSize <= 0 || query.page.pageSize > 200) return Result<TaskPage>::Failure({ErrorCode::InvalidInput, "分页大小必须在 1 到 200 之间"});
    auto created = factory_.Create();
    if (!created.ok()) return Result<TaskPage>::Failure(created.error());
    return created.value()->Tasks().QueryPage(query);
}

Result<HistoryEvent> TaskService::MakeHistoryEvent(IUnitOfWork& uow, const Task& task, OperationType operation, const LocalDateTime& eventAt) {
    HistoryEvent event;
    event.taskId = task.id;
    event.snapshot.title = task.title;
    event.snapshot.note = task.note;
    event.snapshot.priority = task.priority;
    event.snapshot.dueAt = task.dueAt;
    event.snapshot.originalCategoryId = task.categoryId;
    event.snapshot.taskCreatedAt = task.createdAt;
    event.eventAt = eventAt;
    event.completedAt = task.completedAt;
    event.operationType = operation;
    if (task.categoryId) {
        auto category = uow.Categories().GetById(*task.categoryId);
        if (!category.ok()) return Result<HistoryEvent>::Failure(category.error());
        event.snapshot.originalCategoryName = category.value().name;
        event.snapshot.originalCategoryColor = category.value().color;
    }
    return Result<HistoryEvent>::Success(std::move(event));
}

}  // namespace todo
