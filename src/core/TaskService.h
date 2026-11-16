#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "common/Clock.h"
#include "common/Result.h"
#include "core/RepositoryInterfaces.h"
#include "domain/BodyDocument.h"

namespace todo {

struct CreateTaskCommand {
    std::string title;
    std::string note;
    // std::nullopt keeps the existing fast-add behaviour exactly: body_doc stays NULL and note is
    // written as supplied. When present, note is derived from the document by TaskService.
    std::optional<BodyEdit> body;
    Priority priority{Priority::Medium};
    std::optional<LocalDateTime> dueAt;
    std::optional<std::int64_t> categoryId;
};

struct EditTaskCommand {
    std::int64_t id{0};
    std::string title;
    std::string note;
    // std::nullopt means "this caller did not edit the body": body_doc and task_image are left
    // completely untouched. Present means note/body_doc/blobs are rewritten in one UoW.
    std::optional<BodyEdit> body;
    Priority priority{Priority::Medium};
    std::optional<LocalDateTime> dueAt;
    std::optional<std::int64_t> categoryId;
    std::uint64_t expectedRevision{1};
};

class TaskService {
public:
    TaskService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock);
    Result<Task> Create(const CreateTaskCommand& command);
    Result<Task> Edit(const EditTaskCommand& command);
    Result<Task> SetCompleted(std::int64_t id, bool value, std::uint64_t expectedRevision);
    Result<void> Delete(std::int64_t id);
    Result<void> Reorder(const ReorderRequest& command);
    Result<TaskPage> Query(const TaskQuery& query);
    static Result<HistoryEvent> MakeHistoryEvent(IUnitOfWork& uow, const Task& task, OperationType operation, const LocalDateTime& eventAt);

private:
    IUnitOfWorkFactory& factory_;
    std::shared_ptr<IClock> clock_;
};

}  // namespace todo
