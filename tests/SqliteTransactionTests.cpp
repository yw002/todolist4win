// Real SQLite transaction tests. A temporary trigger makes the history INSERT abort, proving that
// "complete the task" and "append the history event" really happen in one transaction and that a
// failure rolls the task row back to pending. Nothing here touches MySQL or the user profile.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <windows.h>

#include "common/Clock.h"
#include "common/Error.h"
#include "core/HistoryService.h"
#include "core/TaskService.h"
#include "data/SqliteConnection.h"
#include "data/SqliteUnitOfWork.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") { ReportTest("SqliteTransaction", name, value, detail); }

class TxClock final : public IClock {
public:
    LocalDateTime now{2026, 7, 1, 8, 0, 0, 0};
    [[nodiscard]] LocalDateTime NowLocal() const override { return now; }
};

std::filesystem::path MakeCaseRoot() {
    static int counter = 0;
    ++counter;
    const std::string name = "todolist4win_sqlite_tx_" + std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" + std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

}  // namespace

void RunSqliteTransactionTests() {
    const auto root = MakeCaseRoot();
    const auto databasePath = root / "data" / "todolist.db";

    SqliteUnitOfWorkFactory factory(databasePath);
    auto ensured = factory.EnsureDatabaseAndSchema();
    Check("schema ensure", ensured.ok(), ensured.ok() ? "" : ensured.error().message.c_str());
    if (!ensured.ok()) { Cleanup(root); return; }

    auto clock = std::make_shared<TxClock>();
    TaskService tasks(factory, clock);

    CreateTaskCommand command;
    command.title = "transactional task";
    command.note = "n";
    command.priority = Priority::Medium;
    auto created = tasks.Create(command);
    Check("fixture task created", created.ok(), created.ok() ? "" : created.error().message.c_str());
    if (!created.ok()) { Cleanup(root); return; }
    const std::int64_t taskId = created.value().id;
    const std::uint64_t revision0 = created.value().revision;

    // Force the history insert to fail so the whole completion must roll back.
    {
        SqliteConnection connection(databasePath);
        auto opened = connection.Open();
        Check("trigger connection opens", opened.ok());
        if (opened.ok()) {
            auto trigger = connection.Exec(
                "CREATE TRIGGER fail_history BEFORE INSERT ON history BEGIN SELECT RAISE(ABORT,'forced history failure'); END;");
            Check("failure trigger created", trigger.ok(), trigger.ok() ? "" : trigger.error().message.c_str());
        }
    }

    auto failed = tasks.SetCompleted(taskId, true, revision0);
    Check("completion fails when the history insert aborts", !failed.ok());

    // The task row must still be pending, at the original revision, with no history.
    {
        TaskQuery query;
        query.route = Route::AllTasks;
        query.page.pageSize = 200;
        query.now = clock->now;
        auto page = tasks.Query(query);
        bool pending = false;
        if (page.ok()) {
            for (const auto& task : page.value().items) {
                if (task.id == taskId) pending = !task.completed && task.revision == revision0;
            }
        }
        Check("task rolled back to pending at the original revision", page.ok() && pending);

        auto uow = factory.Create();
        if (uow.ok()) {
            auto latest = uow.value()->History().GetLatestForTask(taskId);
            Check("no history rows survived the rollback", !latest.ok() && latest.error().code == ErrorCode::NotFound);
        }
    }

    // Remove the trigger: the same transition must now commit task + history atomically.
    {
        SqliteConnection connection(databasePath);
        if (connection.Open().ok()) {
            auto dropped = connection.Exec("DROP TRIGGER fail_history;");
            Check("failure trigger dropped", dropped.ok());
        }
    }
    auto succeeded = tasks.SetCompleted(taskId, true, revision0);
    Check("completion succeeds once the history insert works", succeeded.ok(), succeeded.ok() ? "" : succeeded.error().message.c_str());
    Check("completion sets the flag", succeeded.ok() && succeeded.value().completed);
    {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto latest = uow.value()->History().GetLatestForTask(taskId);
            Check("history event committed", latest.ok());
            Check("committed event is COMPLETE",
                  latest.ok() && latest.value().operationType == OperationType::Complete);
        }
    }

    Cleanup(root);
}
