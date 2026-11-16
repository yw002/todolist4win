#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "common/Result.h"
#include "core/RepositoryInterfaces.h"
#include "domain/BodyDocument.h"

namespace todo {

struct WorkspaceSnapshot {
    TaskPage tasks;
    HistoryPage history;
    std::vector<CategoryCount> categories;
    StatsSnapshot stats;
};

using WorkerValue = std::variant<std::monostate, Task, Category, TaskPage, HistoryPage, std::vector<CategoryCount>, StatsSnapshot, BatchResult, WorkspaceSnapshot, std::vector<BodyImage>>;
using DbCommand = std::function<Result<WorkerValue>(IUnitOfWorkFactory&)>;

struct DbCompletion {
    std::uint64_t requestId{0};
    std::uint64_t generation{0};
    Result<WorkerValue> result{Result<WorkerValue>::Success(WorkerValue{})};
};

class DbWorker {
public:
    DbWorker(std::unique_ptr<IUnitOfWorkFactory> factory, std::function<void()> notify);
    ~DbWorker();
    void Start();
    std::uint64_t Submit(std::uint64_t generation, DbCommand command, bool cancellableRead = false);
    void CancelPendingReads();
    std::vector<DbCompletion> DrainCompletions();
    void Stop();
    IUnitOfWorkFactory& Factory() noexcept;

private:
    struct Request {
        std::uint64_t id;
        std::uint64_t generation;
        DbCommand command;
        bool cancellable;
        bool cancelled;
    };
    void Run(std::stop_token stopToken);
    void PostCancelled(std::uint64_t requestId, std::uint64_t generation);
    std::unique_ptr<IUnitOfWorkFactory> factory_;
    std::function<void()> notify_;
    std::mutex requestMutex_;
    std::condition_variable_any requestCondition_;
    std::deque<Request> requests_;
    std::mutex completionMutex_;
    std::vector<DbCompletion> completions_;
    std::atomic<std::uint64_t> nextId_{1};
    std::jthread worker_;
    bool accepting_{true};
};

}  // namespace todo
