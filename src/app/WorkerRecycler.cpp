#include "app/WorkerRecycler.h"

#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace todo {

WorkerRecycler::WorkerRecycler() : thread_([this](std::stop_token stopToken) { Run(stopToken); }) {}

WorkerRecycler::~WorkerRecycler() { Shutdown(); }

void WorkerRecycler::Retire(std::unique_ptr<DbWorker> worker) {
    if (!worker) return;
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(worker));
    }
    condition_.notify_one();
}

void WorkerRecycler::Run(std::stop_token stopToken) {
    while (true) {
        std::unique_ptr<DbWorker> worker;
        {
            std::unique_lock lock(mutex_);
            // Returns true when work is available. A stop request with an empty queue returns false
            // straight away; a stop request with work left still drains it before exiting, so a
            // retired worker is never leaked to a later (post-shutdown) Retire() call.
            if (!condition_.wait(lock, stopToken, [this] { return !pending_.empty(); })) return;
            worker = std::move(pending_.front());
            pending_.erase(pending_.begin());
        }
        // Stop() joins this worker's own thread; being on the recycler thread is what keeps that
        // join out of the UI thread. The worker is destroyed here as well.
        if (worker) worker->Stop();
    }
}

void WorkerRecycler::Shutdown() {
    thread_.request_stop();
    condition_.notify_all();
    if (thread_.joinable()) thread_.join();
    // Anything retired after the loop ended (or retired while stopping) is stopped here on the
    // caller's thread: this is the last chance before the window and the dispatcher go away.
    std::vector<std::unique_ptr<DbWorker>> leftover;
    {
        std::lock_guard lock(mutex_);
        leftover.swap(pending_);
    }
    for (auto& worker : leftover) {
        if (worker) worker->Stop();
    }
}

}  // namespace todo
