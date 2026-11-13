#pragma once

// In-memory IUnitOfWork implementation used by the offline service tests.
//
// Design notes:
//  * Begin() snapshots the shared database so Rollback() can actually undo writes. Without this
//    the fake would silently keep a half-written state and the "complete + history in one
//    transaction" contract could not be asserted.
//  * Everything here is plain C++: no Win32, no Direct2D, no MySQL Connector, no SQL text. The
//    test project therefore stays linkable without any external dependency.
//  * The fake deliberately does not enforce business rules; TaskService and TaskRules own those.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "common/Clock.h"
#include "core/RepositoryInterfaces.h"

namespace todo::test {

// Fixed clock so tests can assert exact timestamps and can advance time between calls.
class FakeClock final : public IClock {
public:
    LocalDateTime now{2026, 1, 1, 0, 0, 0, 0};
    [[nodiscard]] LocalDateTime NowLocal() const override { return now; }
};

class FakeTaskRepository final : public ITaskRepository {
public:
    std::map<std::int64_t, Task> rows;
    std::int64_t nextId{0};

    Result<Task> Create(const Task& task) override {
        Task stored = task;
        stored.id = ++nextId;
        stored.revision = 1;
        rows[stored.id] = stored;
        return Result<Task>::Success(stored);
    }

    // Mirrors the MySQL repository: optimistic lock on revision, bump on success.
    Result<Task> Update(const Task& task, std::uint64_t expectedRevision) override {
        auto it = rows.find(task.id);
        if (it == rows.end()) return Result<Task>::Failure({ErrorCode::NotFound, "task missing"});
        if (it->second.revision != expectedRevision) return Result<Task>::Failure({ErrorCode::Conflict, "stale revision"});
        Task stored = task;
        stored.revision = expectedRevision + 1;
        it->second = stored;
        return Result<Task>::Success(stored);
    }

    Result<void> DeleteTask(std::int64_t id) override {
        rows.erase(id);
        return Result<void>::Success();
    }

    Result<Task> GetByIdForUpdate(std::int64_t id) override {
        auto it = rows.find(id);
        if (it == rows.end()) return Result<Task>::Failure({ErrorCode::NotFound, "task missing"});
        return Result<Task>::Success(it->second);
    }

    Result<TaskPage> QueryPage(const TaskQuery&) override {
        TaskPage page;
        for (const auto& entry : rows) page.items.push_back(entry.second);
        return Result<TaskPage>::Success(std::move(page));
    }

    Result<void> Reorder(const ReorderRequest&) override { return Result<void>::Success(); }
};

class FakeCategoryRepository final : public ICategoryRepository {
public:
    std::map<std::int64_t, Category> rows;

    Result<Category> Create(const Category& category) override {
        Category stored = category;
        if (stored.id == 0) stored.id = static_cast<std::int64_t>(rows.size()) + 1;
        rows[stored.id] = stored;
        return Result<Category>::Success(stored);
    }

    Result<Category> Update(const Category& category) override {
        auto it = rows.find(category.id);
        if (it == rows.end()) return Result<Category>::Failure({ErrorCode::NotFound, "category missing"});
        it->second = category;
        return Result<Category>::Success(category);
    }

    Result<void> DeleteIfEmpty(std::int64_t id) override {
        rows.erase(id);
        return Result<void>::Success();
    }

    Result<std::vector<CategoryCount>> ListWithCounts() override {
        return Result<std::vector<CategoryCount>>::Success(std::vector<CategoryCount>{});
    }

    Result<Category> GetById(std::int64_t id) override {
        auto it = rows.find(id);
        if (it == rows.end()) return Result<Category>::Failure({ErrorCode::NotFound, "category missing"});
        return Result<Category>::Success(it->second);
    }
};

class FakeHistoryRepository final : public IHistoryRepository {
public:
    std::vector<HistoryEvent> events;
    std::int64_t nextId{0};
    // Force Append() to fail so tests can prove the completion is rolled back as a whole.
    bool failAppend{false};

    Result<HistoryEvent> Append(const HistoryEvent& event) override {
        if (failAppend) return Result<HistoryEvent>::Failure({ErrorCode::QueryFailed, "simulated append failure"});
        HistoryEvent stored = event;
        stored.id = ++nextId;
        events.push_back(stored);
        return Result<HistoryEvent>::Success(stored);
    }

    Result<HistoryPage> QueryPage(const HistoryQuery&) override {
        HistoryPage page;
        page.items = events;
        return Result<HistoryPage>::Success(std::move(page));
    }

    Result<void> DeleteEvents(const std::vector<std::int64_t>& ids) override {
        for (const std::int64_t id : ids) {
            std::erase_if(events, [id](const HistoryEvent& event) { return event.id == id; });
        }
        return Result<void>::Success();
    }

    Result<HistoryEvent> GetLatestForTask(std::int64_t taskId) override {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->taskId == taskId) return Result<HistoryEvent>::Success(*it);
        }
        return Result<HistoryEvent>::Failure({ErrorCode::NotFound, "no history for task"});
    }
};

class FakeStatisticsRepository final : public IStatisticsRepository {
public:
    Result<StatsSnapshot> GetSnapshot(const TimeRange&, const TimeRange&, const LocalDateTime&) override {
        return Result<StatsSnapshot>::Success(StatsSnapshot{});
    }
};

// Snapshot of the mutable parts of the fake database, used to emulate transaction rollback.
struct FakeSnapshot {
    std::map<std::int64_t, Task> tasks;
    std::vector<HistoryEvent> history;
    std::int64_t nextTaskId{0};
    std::int64_t nextHistoryId{0};
};

struct FakeDatabase {
    FakeTaskRepository tasks;
    FakeCategoryRepository categories;
    FakeHistoryRepository history;
    FakeStatisticsRepository statistics;

    // Call counters exposed to tests so transaction boundaries can be asserted.
    int createCount{0};
    int beginCount{0};
    int commitCount{0};
    int rollbackCount{0};
    bool failCommit{false};

    [[nodiscard]] FakeSnapshot Capture() const {
        return FakeSnapshot{tasks.rows, history.events, tasks.nextId, history.nextId};
    }

    void Restore(const FakeSnapshot& snapshot) {
        tasks.rows = snapshot.tasks;
        history.events = snapshot.history;
        tasks.nextId = snapshot.nextTaskId;
        history.nextId = snapshot.nextHistoryId;
    }
};

class FakeUnitOfWork final : public IUnitOfWork {
public:
    explicit FakeUnitOfWork(FakeDatabase& database) : database_(database) {}

    Result<void> Begin() override {
        snapshot_ = database_.Capture();
        ++database_.beginCount;
        return Result<void>::Success();
    }

    Result<void> Commit() override {
        if (database_.failCommit) return Result<void>::Failure({ErrorCode::QueryFailed, "simulated commit failure"});
        snapshot_.reset();
        ++database_.commitCount;
        return Result<void>::Success();
    }

    void Rollback() noexcept override {
        if (snapshot_) database_.Restore(*snapshot_);
        snapshot_.reset();
        ++database_.rollbackCount;
    }

    ITaskRepository& Tasks() override { return database_.tasks; }
    ICategoryRepository& Categories() override { return database_.categories; }
    IHistoryRepository& History() override { return database_.history; }
    IStatisticsRepository& Statistics() override { return database_.statistics; }

private:
    FakeDatabase& database_;
    std::optional<FakeSnapshot> snapshot_;
};

class FakeUnitOfWorkFactory final : public IUnitOfWorkFactory {
public:
    Result<std::unique_ptr<IUnitOfWork>> Create() override {
        ++database_.createCount;
        return Result<std::unique_ptr<IUnitOfWork>>::Success(std::make_unique<FakeUnitOfWork>(database_));
    }

    // T14: lets a data source switch test make the schema probe fail without breaking the rest of
    // the fake, so "candidate probe failed -> old data source stays" is deterministic. The flag
    // lives on the factory (not on FakeDatabase) so a test that keeps a raw pointer to the factory
    // can flip it between attempts - the factory object itself never moves, only the unique_ptr.
    Result<void> EnsureDatabaseAndSchema() override {
        if (failEnsure) return Result<void>::Failure({ErrorCode::ConnectionFailed, "simulated schema probe failure"});
        return Result<void>::Success();
    }

    [[nodiscard]] FakeDatabase& Db() noexcept { return database_; }
    [[nodiscard]] const FakeDatabase& Db() const noexcept { return database_; }

    bool failEnsure{false};

private:
    FakeDatabase database_;
};

}  // namespace todo::test
