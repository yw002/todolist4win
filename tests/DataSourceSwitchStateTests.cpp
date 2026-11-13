// Data source hot-switch state machine (design 6.2 / 7 / 13.3).
//
// These tests drive a real AppController + real DbWorker pair against the in-memory
// FakeUnitOfWorkFactory, so the ownership hand-over, the pending/cache reset and the retirement of
// the previous worker are exercised exactly as the window does it - only the message loop is
// replaced by an explicit OnCompletions() pump. Nothing here touches a database, DPAPI, %APPDATA%
// or %LOCALAPPDATA%: the candidate factories and the logger all point at a temporary directory.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "FakeUnitOfWork.h"
#include "app/AppController.h"
#include "app/Logger.h"
#include "common/Clock.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") { ReportTest("DataSourceSwitch", name, value, detail); }

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_switch_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" + std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

Task MakeTask(std::int64_t id, const std::string& title) {
    Task task;
    task.id = id;
    task.title = title;
    task.createdAt = LocalDateTime{2026, 1, 1, 8, 0, 0, 0};
    task.sortWeight = static_cast<double>(id);
    return task;
}

HistoryEvent MakeHistoryEvent(std::int64_t id, std::int64_t taskId) {
    HistoryEvent event;
    event.id = id;
    event.taskId = taskId;
    event.snapshot.title = "history-" + std::to_string(taskId);
    event.eventAt = LocalDateTime{2026, 1, 1, 9, 0, 0, 0};
    event.operationType = OperationType::Complete;
    return event;
}

DataSourceDraft MySqlDraft() {
    DataSourceDraft draft;
    draft.kind = DataSourceKind::MySql;
    draft.database.host = "db.example";
    draft.database.port = 3307;
    draft.database.username = "app";
    draft.database.database = "todolist";
    draft.passwordAction = PasswordAction::Keep;
    return draft;
}

// Mirrors the window's message loop: the worker only posts a notification, so draining completions
// on this thread is what actually advances the controller's state.
bool Pump(AppController& controller, const std::function<bool()>& done, int timeoutMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        controller.OnCompletions();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    controller.OnCompletions();
    return done();
}

}  // namespace

void RunDataSourceSwitchStateTests() {
    // --- 1. Startup mirrors the configured source and credential state ------
    const auto stateRoot = MakeCaseRoot("state");
    {
        Logger logger(stateRoot / L"test.log");
        AppConfig config;  // Local by default
        auto factory = std::make_unique<test::FakeUnitOfWorkFactory>();
        factory->Db().tasks.rows[1] = MakeTask(1, "local-task");
        auto worker = std::make_unique<DbWorker>(std::move(factory), [] {});
        bool hasSaved = true;
        int changes = 0;
        DataSourceSwitchPort port;
        port.savedCredential = [&hasSaved] { return hasSaved; };
        AppController controller(config, std::move(worker), std::make_shared<SystemClock>(), logger, [&changes] { ++changes; }, port);
        controller.Start();
        Check("initial source mirrors the config", controller.State().dataSource == DataSourceKind::Local);
        Check("initial credential state comes from the port", controller.State().hasSavedCredential);
        Check("first load reaches online", Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));
        Check("first load clears read-only", !controller.State().readOnly);
        Check("first load loads the rows", controller.State().tasks.size() == 1 && controller.State().tasks[0].title == "local-task");
        Check("no switch is reported at rest", !controller.State().switching);
        Check("state changes are reported to the window", changes > 0);
    }
    Cleanup(stateRoot);

    // --- 2. A failing candidate probe changes nothing -----------------------
    const auto failRoot = MakeCaseRoot("probe-fail");
    {
        Logger logger(failRoot / L"test.log");
        AppConfig config;
        auto factory = std::make_unique<test::FakeUnitOfWorkFactory>();
        auto* factoryRaw = factory.get();
        factoryRaw->Db().tasks.rows[1] = MakeTask(1, "local-task");
        auto worker = std::make_unique<DbWorker>(std::move(factory), [] {});
        int candidateCount = 0;
        int commitCount = 0;
        DataSourceSwitchPort port;
        port.savedCredential = [] { return false; };
        port.createCandidate = [&candidateCount](const DataSourceDraft&) -> Result<std::unique_ptr<DbWorker>> {
            ++candidateCount;
            auto candidate = std::make_unique<test::FakeUnitOfWorkFactory>();
            candidate->failEnsure = true;  // the probe itself fails
            return Result<std::unique_ptr<DbWorker>>::Success(std::make_unique<DbWorker>(std::move(candidate), [] {}));
        };
        port.commit = [&commitCount](const DataSourceDraft&) -> Result<DataSourceCommitResult> {
            ++commitCount;
            return Result<DataSourceCommitResult>::Success(DataSourceCommitResult{});
        };
        port.retire = [](std::unique_ptr<DbWorker>) {};  // dropped: the old worker is idle here
        AppController controller(config, std::move(worker), std::make_shared<SystemClock>(), logger, [] {}, port);
        controller.Start();
        Check("probe-fail session loads", Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));
        const std::uint64_t generationBefore = controller.State().generation;
        controller.RequestDataSourceSwitch(MySqlDraft());
        Check("the switch starts synchronously", controller.State().switching);
        Check("exactly one candidate was created", candidateCount == 1);
        Check("read-only is forced while switching", controller.State().readOnly);
        Check("the failed switch finishes", Pump(controller, [&] { return !controller.State().switching; }));
        Check("the failed switch never commits", commitCount == 0);
        Check("the failed switch keeps the data source",
              controller.State().dataSource == DataSourceKind::Local && controller.Config().dataSource == DataSourceKind::Local);
        Check("the failed switch keeps the visible rows",
              controller.State().tasks.size() == 1 && controller.State().tasks[0].title == "local-task");
        Check("the failed switch restores write access", !controller.State().readOnly);
        Check("the failed switch reports the error", !controller.State().errorMessage.empty());
        Check("the failed switch says the old source is kept",
              controller.State().toast.find("仍使用旧数据源") != std::string::npos);
        Check("the failed switch does not bump the generation", controller.State().generation == generationBefore);
        Check("the failed switch leaves the old store untouched", factoryRaw->Db().tasks.rows.size() == 1);
    }
    Cleanup(failRoot);

    // --- 3. A successful probe commits, then adopts and resets the view -----
    const auto okRoot = MakeCaseRoot("probe-ok");
    {
        Logger logger(okRoot / L"test.log");
        AppConfig config;
        config.window.topmost = true;
        config.window.widthDip = 1234.0;
        auto factory = std::make_unique<test::FakeUnitOfWorkFactory>();
        auto* factoryRaw = factory.get();
        factoryRaw->Db().tasks.rows[1] = MakeTask(1, "local-task");
        factoryRaw->Db().history.events.push_back(MakeHistoryEvent(1, 1));
        auto worker = std::make_unique<DbWorker>(std::move(factory), [] {});
        std::vector<std::unique_ptr<DbWorker>> retired;
        int commitCount = 0;
        DataSourceSwitchPort port;
        port.savedCredential = [] { return true; };
        port.createCandidate = [](const DataSourceDraft&) -> Result<std::unique_ptr<DbWorker>> {
            auto candidate = std::make_unique<test::FakeUnitOfWorkFactory>();
            candidate->Db().tasks.rows[99] = MakeTask(99, "mysql-task");
            return Result<std::unique_ptr<DbWorker>>::Success(std::make_unique<DbWorker>(std::move(candidate), [] {}));
        };
        port.commit = [&commitCount](const DataSourceDraft& draft) -> Result<DataSourceCommitResult> {
            ++commitCount;
            DataSourceCommitResult result;
            result.config.dataSource = draft.kind;
            result.config.database = draft.database;
            result.hasSavedCredential = true;
            return Result<DataSourceCommitResult>::Success(result);
        };
        port.retire = [&retired](std::unique_ptr<DbWorker> worker) {
            if (worker) retired.push_back(std::move(worker));
        };
        AppController controller(config, std::move(worker), std::make_shared<SystemClock>(), logger, [] {}, port);
        controller.Start();
        Check("adoption session loads", Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));
        Check("the old source has history rows", controller.State().history.size() == 1);
        controller.SetThemeMode(ThemeMode::Dark);
        controller.SetTopmost(true);
        controller.Navigate(Route::History);
        controller.SetKeyword("needle");
        controller.FlushPendingSearch(true);
        const std::uint64_t generationBefore = controller.State().generation;

        controller.RequestDataSourceSwitch(MySqlDraft());
        Check("the switch is running", controller.State().switching);
        // The state machine only advances in OnCompletions(), so this window is deterministic: the
        // probe may already be running, but nothing can be adopted until the UI thread drains it.
        const int createsBefore = factoryRaw->Db().createCount;
        CreateTaskCommand refused;
        refused.title = "should-not-land";
        controller.CreateTask(refused);
        Check("writes are refused during the switch",
              controller.State().toast.find("切换数据源") != std::string::npos);
        Check("the refused write never reached the old store", factoryRaw->Db().createCount == createsBefore);
        Check("the switch completes", Pump(controller, [&] { return !controller.State().switching; }));
        Check("the commit happened exactly once", commitCount == 1);
        Check("the new source is adopted",
              controller.State().dataSource == DataSourceKind::MySql && controller.Config().dataSource == DataSourceKind::MySql);
        Check("the adopted source is online", controller.State().connection == ConnectionState::Online);
        Check("the route is reset to all tasks",
              controller.State().route == Route::AllTasks && !controller.State().categoryId.has_value());
        Check("the filters are reset",
              controller.State().filter.keyword.empty() && controller.State().historyFilter.keyword.empty());
        Check("the old source's history is gone", controller.State().history.empty());
        Check("the cursors are cleared", !controller.State().taskCursor.has_value() && !controller.State().hasTaskNext &&
                                              !controller.State().historyCursor.has_value() && !controller.State().hasHistoryNext);
        Check("only the new source's rows are visible",
              controller.State().tasks.size() == 1 && controller.State().tasks[0].title == "mysql-task");
        Check("nothing was merged from the old source",
              std::none_of(controller.State().tasks.begin(), controller.State().tasks.end(),
                           [](const Task& task) { return task.title == "local-task"; }));
        Check("the overlay and selection are cleared",
              controller.State().overlay == Overlay::None && controller.State().focusedControl == FocusedControl::None);
        Check("the theme preference survives", controller.State().themeMode == ThemeMode::Dark);
        Check("the topmost preference survives", controller.State().topmost);
        Check("the window preferences survive", controller.Config().window.widthDip == 1234.0);
        Check("the generation was bumped", controller.State().generation > generationBefore);
        Check("the credential state follows the commit", controller.State().hasSavedCredential);
        Check("the previous worker was retired", retired.size() == 1);

        // --- 3b. The retired worker's completions cannot reach the new state ----
        // Both workers number their requests from 1, so an id collision is guaranteed. A completion
        // that is still in the retired queue must stay invisible to the controller.
        const std::uint64_t staleGeneration = controller.State().generation;
        const std::uint64_t staleId = retired.front()->Submit(staleGeneration, [](IUnitOfWorkFactory&) -> Result<WorkerValue> {
            return Result<WorkerValue>::Failure({ErrorCode::QueryFailed, "stale completion from the old source"});
        }, false);
        for (int i = 0; i < 50; ++i) {
            controller.OnCompletions();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check("a stale completion cannot change the current state",
              controller.State().errorMessage.empty() && controller.State().connection == ConnectionState::Online);
        Check("a stale completion cannot reopen the old rows",
              controller.State().tasks.size() == 1 && controller.State().tasks[0].title == "mysql-task");
        const auto leftover = retired.front()->DrainCompletions();
        Check("the stale completion stayed in the retired worker's queue",
              std::any_of(leftover.begin(), leftover.end(),
                          [staleId](const DbCompletion& completion) { return completion.requestId == staleId; }));
    }
    Cleanup(okRoot);

    // --- 4. An unresolvable credential aborts before any candidate exists ---
    const auto portRoot = MakeCaseRoot("port");
    {
        Logger logger(portRoot / L"test.log");
        AppConfig config;
        auto factory = std::make_unique<test::FakeUnitOfWorkFactory>();
        auto worker = std::make_unique<DbWorker>(std::move(factory), [] {});
        int candidateCount = 0;
        int commitCount = 0;
        bool failCreate = false;
        DataSourceSwitchPort port;
        port.savedCredential = [] { return true; };
        port.createCandidate = [&candidateCount, &failCreate](const DataSourceDraft&) -> Result<std::unique_ptr<DbWorker>> {
            ++candidateCount;
            if (failCreate) return Result<std::unique_ptr<DbWorker>>::Failure({ErrorCode::CredentialUnavailable, "vault"});
            auto candidate = std::make_unique<test::FakeUnitOfWorkFactory>();
            return Result<std::unique_ptr<DbWorker>>::Success(std::make_unique<DbWorker>(std::move(candidate), [] {}));
        };
        port.commit = [&commitCount](const DataSourceDraft&) -> Result<DataSourceCommitResult> {
            ++commitCount;
            return Result<DataSourceCommitResult>::Success(DataSourceCommitResult{});
        };
        port.retire = [](std::unique_ptr<DbWorker>) {};
        AppController controller(config, std::move(worker), std::make_shared<SystemClock>(), logger, [] {}, port);
        controller.Start();
        Check("port session loads", Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));

        failCreate = true;
        controller.RequestDataSourceSwitch(MySqlDraft());
        Check("an unreadable credential aborts the switch at once", !controller.State().switching);
        Check("an unreadable credential keeps the old source",
              controller.State().dataSource == DataSourceKind::Local && controller.Config().dataSource == DataSourceKind::Local);
        Check("an unreadable credential asks for the password again",
              controller.State().toast.find("重新输入") != std::string::npos);
        Check("an unreadable credential never commits", commitCount == 0);
        Check("an unreadable credential leaves write access alone", !controller.State().readOnly);

        failCreate = false;
        controller.RequestDataSourceSwitch(MySqlDraft());
        const int candidatesAfterFirst = candidateCount;
        controller.RequestDataSourceSwitch(MySqlDraft());
        Check("a second switch is ignored while one is in flight", candidateCount == candidatesAfterFirst);
        Check("the first switch still finishes", Pump(controller, [&] { return !controller.State().switching; }));

        // A local switch must not be blocked by the MySQL credential at all.
        DataSourceDraft localDraft;
        localDraft.kind = DataSourceKind::Local;
        controller.RequestDataSourceSwitch(localDraft);
        Check("a local switch is accepted", Pump(controller, [&] { return !controller.State().switching; }));
        Check("a local switch is adopted", controller.State().dataSource == DataSourceKind::Local);
    }
    Cleanup(portRoot);

    // --- 5. Retry reuses the open worker instead of rebuilding a connection -
    const auto retryRoot = MakeCaseRoot("retry");
    {
        Logger logger(retryRoot / L"test.log");
        AppConfig config;
        auto factory = std::make_unique<test::FakeUnitOfWorkFactory>();
        auto* factoryRaw = factory.get();
        factoryRaw->Db().tasks.rows[1] = MakeTask(1, "local-task");
        auto worker = std::make_unique<DbWorker>(std::move(factory), [] {});
        int candidateCount = 0;
        DataSourceSwitchPort port;
        port.savedCredential = [] { return false; };
        port.createCandidate = [&candidateCount](const DataSourceDraft&) -> Result<std::unique_ptr<DbWorker>> {
            ++candidateCount;
            auto candidate = std::make_unique<test::FakeUnitOfWorkFactory>();
            return Result<std::unique_ptr<DbWorker>>::Success(std::make_unique<DbWorker>(std::move(candidate), [] {}));
        };
        port.commit = [](const DataSourceDraft&) { return Result<DataSourceCommitResult>::Success(DataSourceCommitResult{}); };
        port.retire = [](std::unique_ptr<DbWorker>) {};
        AppController controller(config, std::move(worker), std::make_shared<SystemClock>(), logger, [] {}, port);
        controller.Start();
        Check("retry session loads", Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));

        factoryRaw->failEnsure = true;
        controller.Retry();
        Check("retry switches the status to reconnecting", controller.State().connection == ConnectionState::Reconnecting);
        Check("the failing retry finishes",
              Pump(controller, [&] { return controller.State().connection != ConnectionState::Reconnecting; }));
        Check("a failing retry degrades the connection",
              controller.State().connection == ConnectionState::Degraded && controller.State().readOnly);
        Check("a failing retry keeps the last rows", controller.State().tasks.size() == 1);

        factoryRaw->failEnsure = false;
        controller.Retry();
        Check("retry recovers on the same worker",
              Pump(controller, [&] { return controller.State().connection == ConnectionState::Online; }));
        Check("retry never creates a candidate", candidateCount == 0);
        Check("retry keeps the same rows", controller.State().tasks.size() == 1);
    }
    Cleanup(retryRoot);
}
