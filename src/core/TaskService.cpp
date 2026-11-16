#include "core/TaskService.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/ImageValidation.h"
#include "domain/BodyDocument.h"
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

// The one and only place where note is derived from a body document. Repositories never derive it;
// they persist exactly what they are given. Keeping the canonical JSON here also means both data
// sources store byte-identical documents for the same edit.
struct ResolvedBody {
    std::string note;
    std::string docJson;
    std::vector<std::string> keys;
    std::vector<BodyImage> images;
};

[[nodiscard]] Result<ResolvedBody> ResolveBody(const BodyEdit& body) {
    auto parsed = BodyDocument::Parse(body.doc);
    if (!parsed.ok()) return Result<ResolvedBody>::Failure(parsed.error());
    const auto validated = parsed.value().Validate();
    if (!validated.ok()) return Result<ResolvedBody>::Failure(validated.error());
    const auto images = ValidateNewImages(body.newImages);
    if (!images.ok()) return Result<ResolvedBody>::Failure(images.error());
    ResolvedBody resolved;
    resolved.note = parsed.value().PlainText(false);
    resolved.docJson = parsed.value().ToJson();
    resolved.keys = parsed.value().ImageKeys();
    resolved.images = body.newImages;
    const auto note = TaskRules::ValidateNote(resolved.note);
    if (!note.ok()) return Result<ResolvedBody>::Failure(note.error());
    return Result<ResolvedBody>::Success(std::move(resolved));
}

// Writes the new blobs and removes those no longer referenced, all inside the caller's UoW. A
// failure anywhere here propagates to the TransactionGuard, which rolls back note + body_doc too.
[[nodiscard]] Result<void> SyncImages(IUnitOfWork& uow, std::int64_t taskId, const ResolvedBody& resolved) {
    for (const BodyImage& image : resolved.images) {
        const auto put = uow.Images().Put(taskId, image);
        if (!put.ok()) return put;
    }
    return uow.Images().DeleteUnreferenced(taskId, resolved.keys);
}

// History keeps a pure-text snapshot. A missing legacy body_doc and an unparseable/newer document
// both fall back to the plain note: a bad document must never make a history row fail to append.
[[nodiscard]] std::string HistoryNote(const Task& task) {
    if (task.bodyDoc.empty()) return task.note;
    const auto parsed = BodyDocument::Parse(task.bodyDoc);
    if (!parsed.ok()) return task.note;
    return parsed.value().PlainText(true);
}

}  // namespace

TaskService::TaskService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock) : factory_(factory), clock_(std::move(clock)) {}

Result<Task> TaskService::Create(const CreateTaskCommand& command) {
    auto title = TaskRules::ValidateTitle(command.title);
    if (!title.ok()) return Result<Task>::Failure(title.error());
    auto category = ValidateCategoryId(command.categoryId);
    if (!category.ok()) return Result<Task>::Failure(category.error());
    std::optional<ResolvedBody> resolved;
    if (command.body) {
        auto body = ResolveBody(*command.body);
        if (!body.ok()) return Result<Task>::Failure(body.error());
        resolved = std::move(body.value());
    } else {
        auto note = TaskRules::ValidateNote(command.note);
        if (!note.ok()) return Result<Task>::Failure(note.error());
    }
    auto created = factory_.Create();
    if (!created.ok()) return Result<Task>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Task>::Failure(begun.error());
    TransactionGuard guard(uow);
    Task task;
    task.title = TaskRules::Trim(command.title);
    task.note = resolved ? resolved->note : command.note;
    if (resolved) task.bodyDoc = resolved->docJson;
    task.priority = command.priority;
    task.dueAt = command.dueAt;
    task.categoryId = command.categoryId;
    task.createdAt = clock_->NowLocal();
    auto result = uow.Tasks().Create(task);
    if (!result.ok()) return result;
    if (resolved) {
        const auto synced = SyncImages(uow, result.value().id, *resolved);
        if (!synced.ok()) return Result<Task>::Failure(synced.error());
    }
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
    auto category = ValidateCategoryId(command.categoryId);
    if (!category.ok()) return Result<Task>::Failure(category.error());
    std::optional<ResolvedBody> resolved;
    if (command.body) {
        auto body = ResolveBody(*command.body);
        if (!body.ok()) return Result<Task>::Failure(body.error());
        resolved = std::move(body.value());
    } else {
        auto note = TaskRules::ValidateNote(command.note);
        if (!note.ok()) return Result<Task>::Failure(note.error());
    }
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
    updated.note = resolved ? resolved->note : command.note;
    // Without a body edit, bodyDoc is exactly what the repository just read (including an
    // unparseable v>1 document) and is written back verbatim - never downgraded or erased.
    if (resolved) updated.bodyDoc = resolved->docJson;
    updated.priority = command.priority;
    updated.dueAt = command.dueAt;
    updated.categoryId = command.categoryId;
    auto result = uow.Tasks().Update(updated, command.expectedRevision);
    if (!result.ok()) return result;
    if (resolved) {
        const auto synced = SyncImages(uow, command.id, *resolved);
        if (!synced.ok()) return Result<Task>::Failure(synced.error());
    }
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
    // Text-only projection with 〔图片〕 markers; history never references task_image rows, so
    // deleting a task can cascade its history and blobs without leaving orphans.
    event.snapshot.note = HistoryNote(task);
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
