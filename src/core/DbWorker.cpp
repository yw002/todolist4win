#include "core/DbWorker.h"

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace todo {

DbWorker::DbWorker(std::unique_ptr<IUnitOfWorkFactory> factory, std::function<void()> notify) : factory_(std::move(factory)), notify_(std::move(notify)) {}
DbWorker::~DbWorker() { Stop(); }

void DbWorker::Start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token token) { Run(token); });
}

std::uint64_t DbWorker::Submit(std::uint64_t generation, DbCommand command, bool cancellableRead) {
    const std::uint64_t id = nextId_.fetch_add(1);
    bool accepted = false;
    {
        std::lock_guard lock(requestMutex_);
        if (accepting_) {
            requests_.push_back({id, generation, std::move(command), cancellableRead, false});
            accepted = true;
        }
    }
    if (!accepted) {
        // Every allocated request id must produce exactly one completion, otherwise the caller's
        // pending table leaks the entry forever and stops refreshing.
        PostCancelled(id, generation);
        return id;
    }
    requestCondition_.notify_one();
    return id;
}

void DbWorker::PostCancelled(std::uint64_t requestId, std::uint64_t generation) {
    {
        std::lock_guard lock(completionMutex_);
        completions_.push_back({requestId, generation, Result<WorkerValue>::Failure({ErrorCode::Cancelled, "请求已取消"})});
    }
    if (notify_) notify_();
}

void DbWorker::CancelPendingReads() {
    std::lock_guard lock(requestMutex_);
    for (auto& request : requests_) if (request.cancellable) request.cancelled = true;
}

std::vector<DbCompletion> DbWorker::DrainCompletions() {
    std::lock_guard lock(completionMutex_);
    std::vector<DbCompletion> result;
    result.swap(completions_);
    return result;
}

void DbWorker::Stop() {
    std::deque<Request> dropped;
    {
        std::lock_guard lock(requestMutex_);
        accepting_ = false;
        dropped.swap(requests_);
    }
    requestCondition_.notify_all();
    // Report the dropped requests so no caller waits on a completion that will never come.
    for (const auto& request : dropped) PostCancelled(request.id, request.generation);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

IUnitOfWorkFactory& DbWorker::Factory() noexcept { return *factory_; }

void DbWorker::Run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        Request request{};
        {
            std::unique_lock lock(requestMutex_);
            requestCondition_.wait(lock, stopToken, [this] { return !accepting_ || !requests_.empty(); });
            if (!accepting_ || requests_.empty()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        if (request.cancelled) {
            // Superseded reads still owe the caller a completion; the UI drops it silently.
            PostCancelled(request.id, request.generation);
            continue;
        }
        Result<WorkerValue> result = Result<WorkerValue>::Failure({ErrorCode::Internal, "数据库命令未执行"});
        try {
            result = request.command(*factory_);
        } catch (const std::exception&) {
            result = Result<WorkerValue>::Failure({ErrorCode::Internal, "数据库操作发生内部错误"});
        } catch (...) {
            result = Result<WorkerValue>::Failure({ErrorCode::Internal, "数据库操作发生未知错误"});
        }
        {
            std::lock_guard lock(completionMutex_);
            completions_.push_back({request.id, request.generation, std::move(result)});
        }
        if (notify_) notify_();
    }
}

}  // namespace todo
