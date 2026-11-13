#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/DbWorker.h"

namespace todo {

// Stops DbWorker instances that are no longer the current one, off the UI thread.
//
// Why this exists (design 6.3): after a data source switch the previous worker may still be inside a
// blocking query (a MySQL connect can legitimately take seconds). DbWorker::Stop() joins that
// thread, so calling it from the UI thread would freeze the window at exactly the moment the user
// expects the new source to appear. AppController therefore hands the retired worker over here and
// forgets about it.
//
// This is also why a stale completion can never reach the new UI: the retired worker's queue is
// never drained again, and its notification callback only posts a message to the window - it does
// not touch controller state. The recycler is joined before the dispatcher is detached, so no late
// post can target a destroyed window.
class WorkerRecycler {
public:
    WorkerRecycler();
    ~WorkerRecycler();
    WorkerRecycler(const WorkerRecycler&) = delete;
    WorkerRecycler& operator=(const WorkerRecycler&) = delete;

    // Takes ownership and returns immediately; the Stop() happens on the recycler thread.
    void Retire(std::unique_ptr<DbWorker> worker);

    // Stops the recycler thread and every worker it still owns. Idempotent, and safe to call from
    // the UI thread once the message loop has ended.
    void Shutdown();

private:
    void Run(std::stop_token stopToken);
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::unique_ptr<DbWorker>> pending_;
    std::jthread thread_;
};

}  // namespace todo
