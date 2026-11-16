#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "common/Result.h"
#include "domain/BodyDocument.h"
#include "domain/Models.h"

namespace todo {

struct ReorderRequest {
    std::int64_t taskId{0};
    std::optional<std::int64_t> previousId;
    std::optional<std::int64_t> nextId;
    std::optional<double> previousWeight;
    std::optional<double> nextWeight;
};

class ITaskRepository {
public:
    virtual ~ITaskRepository() = default;
    virtual Result<Task> Create(const Task& task) = 0;
    virtual Result<Task> Update(const Task& task, std::uint64_t expectedRevision) = 0;
    virtual Result<void> DeleteTask(std::int64_t id) = 0;
    virtual Result<Task> GetByIdForUpdate(std::int64_t id) = 0;
    virtual Result<TaskPage> QueryPage(const TaskQuery& request) = 0;
    virtual Result<void> Reorder(const ReorderRequest& request) = 0;
};

class ICategoryRepository {
public:
    virtual ~ICategoryRepository() = default;
    virtual Result<Category> Create(const Category& category) = 0;
    virtual Result<Category> Update(const Category& category) = 0;
    virtual Result<void> DeleteIfEmpty(std::int64_t id) = 0;
    virtual Result<std::vector<CategoryCount>> ListWithCounts() = 0;
    virtual Result<Category> GetById(std::int64_t id) = 0;
};

class IHistoryRepository {
public:
    virtual ~IHistoryRepository() = default;
    virtual Result<HistoryEvent> Append(const HistoryEvent& event) = 0;
    virtual Result<HistoryPage> QueryPage(const HistoryQuery& request) = 0;
    virtual Result<void> DeleteEvents(const std::vector<std::int64_t>& ids) = 0;
    virtual Result<HistoryEvent> GetLatestForTask(std::int64_t taskId) = 0;
};

class IStatisticsRepository {
public:
    virtual ~IStatisticsRepository() = default;
    virtual Result<StatsSnapshot> GetSnapshot(const TimeRange& today, const TimeRange& sevenDays, const LocalDateTime& now) = 0;
};

// PNG blobs live in task_image. `Get` is a single-row read used only while the editor is open;
// list/paging queries must never touch this repository or JOIN its table.
class ITaskImageRepository {
public:
    virtual ~ITaskImageRepository() = default;
    // Idempotent: an existing (taskId, image.key) is left byte-for-byte unchanged.
    virtual Result<void> Put(std::int64_t taskId, const BodyImage& image) = 0;
    virtual Result<BodyImage> Get(std::int64_t taskId, const std::string& key) = 0;
    // Deletes every blob of this task whose key is not in `keys`. An empty vector deletes all of
    // them. A key referenced by two blocks appears once in ImageKeys() and is therefore kept.
    virtual Result<void> DeleteUnreferenced(std::int64_t taskId, const std::vector<std::string>& keys) = 0;
};

class IUnitOfWork {
public:
    virtual ~IUnitOfWork() = default;
    virtual Result<void> Begin() = 0;
    virtual Result<void> Commit() = 0;
    virtual void Rollback() noexcept = 0;
    virtual ITaskRepository& Tasks() = 0;
    virtual ICategoryRepository& Categories() = 0;
    virtual IHistoryRepository& History() = 0;
    virtual IStatisticsRepository& Statistics() = 0;
    virtual ITaskImageRepository& Images() = 0;
};

class IUnitOfWorkFactory {
public:
    virtual ~IUnitOfWorkFactory() = default;
    virtual Result<std::unique_ptr<IUnitOfWork>> Create() = 0;
    virtual Result<void> EnsureDatabaseAndSchema() = 0;
};

class TransactionGuard {
public:
    explicit TransactionGuard(IUnitOfWork& uow) : uow_(uow) {}
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    ~TransactionGuard() { if (!committed_) uow_.Rollback(); }
    void Committed() noexcept { committed_ = true; }
private:
    IUnitOfWork& uow_;
    bool committed_{false};
};

}  // namespace todo
