#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "app/AppState.h"
#include "common/Clock.h"
#include "config/AppConfig.h"
#include "core/DbWorker.h"
#include "core/HistoryService.h"
#include "core/TaskService.h"
#include "app/Logger.h"

namespace todo {

struct NavigateIntent { Route route; std::optional<std::int64_t> categoryId; };
struct KeywordIntent { std::string keyword; };
struct HistoryKeywordIntent { std::string keyword; };
struct ToggleCompletionIntent { std::int64_t taskId; bool completed; std::uint64_t revision; };
struct DeleteTaskIntent { std::int64_t taskId; };
struct RestoreTaskIntent { std::int64_t taskId; };
struct DeleteHistoryIntent { std::vector<std::int64_t> eventIds; };
using AppIntent = std::variant<NavigateIntent, KeywordIntent, HistoryKeywordIntent, CreateTaskCommand, EditTaskCommand, ToggleCompletionIntent, DeleteTaskIntent, RestoreTaskIntent, DeleteHistoryIntent, ReorderRequest>;

// What a successful commit hands back to the controller.
struct DataSourceCommitResult {
    AppConfig config;               // effective configuration (password resolved, never persisted)
    bool hasSavedCredential{false}; // mirrors SettingsService::Credential() == Present
};

// The three operations a hot switch needs from the composition root. AppController owns the state
// machine (probe -> commit -> adoption) and the UI reset, but it never touches DPAPI, ConfigStore or
// a concrete factory: those live in Application, which is the only place that knows how to build a
// worker for a given data source (design 6.1).
//
// All of them are std::function so the switching logic stays testable without Win32 or a real
// database - a test can plug in a fake factory that fails or succeeds on demand.
struct DataSourceSwitchPort {
    // Build (do not start) the candidate worker for the draft. The password is already resolved by
    // SettingsService: CredentialUnavailable means the stored secret cannot be decrypted, and then
    // no candidate is created at all.
    std::function<Result<std::unique_ptr<DbWorker>>(const DataSourceDraft& draft)> createCandidate;
    // Persist credential + INI after the probe succeeded. Returns the effective configuration, or an
    // error (in which case nothing was switched and the old source is still fully usable).
    std::function<Result<DataSourceCommitResult>(const DataSourceDraft& draft)> commit;
    // Hand a worker that is no longer current to a background recycler. Must return immediately: it
    // is called from the UI thread and Stop() joins a thread that may be inside a query.
    std::function<void(std::unique_ptr<DbWorker>)> retire;
    // Cheap read of the current credential state (saved / not saved). Never decrypts twice: it only
    // reports what SettingsService already knows.
    std::function<bool()> savedCredential;
};

class AppController {
public:
    AppController(AppConfig config, std::unique_ptr<DbWorker> worker, std::shared_ptr<IClock> clock, Logger& logger, std::function<void()> changed, DataSourceSwitchPort port = {});
    ~AppController();
    void Start();
    void Dispatch(const AppIntent& intent);
    void OnCompletions();
    // Retries the current data source on the worker / factory that is already open. This is what the
    // status-bar retry button calls: Config() carries no password, so rebuilding a connection from
    // it is impossible - only a candidate switch may change the connection parameters.
    void Retry();
    // Full data source switch (design 6.2). The probe runs on a candidate worker while the current
    // one keeps serving; the configuration and credential are written only after the probe succeeds.
    void RequestDataSourceSwitch(const DataSourceDraft& draft);
    void Navigate(Route route, std::optional<std::int64_t> categoryId = std::nullopt);
    void SetKeyword(const std::string& keyword);
    void SetHistoryKeyword(const std::string& keyword);
    void CreateTask(const CreateTaskCommand& command);
    void EditTask(const EditTaskCommand& command);
    void SetCompleted(std::int64_t id, bool completed, std::uint64_t revision);
    void DeleteTask(std::int64_t id);
    void Reorder(const ReorderRequest& request);
    void CreateCategory(const std::string& name, const std::string& color);
    void UpdateCategory(std::int64_t id, const std::string& name, const std::string& color);
    void DeleteCategory(std::int64_t id);
    void Restore(std::int64_t taskId);
    void RestoreBatch(const std::vector<std::int64_t>& ids);
    void DeleteHistory(const std::vector<std::int64_t>& ids);
    void LoadMore();
    void Refresh();
    void Tick();
    bool FlushPendingSearch(bool force = false);
    void ShowToast(const std::string& message);
    void SetCompletionDisplay(CompletionDisplay display);
    void SetTopmost(bool value) noexcept;
    void SetThemeMode(ThemeMode mode) noexcept;
    void ClearToast();
    [[nodiscard]] AppState& State() noexcept;
    [[nodiscard]] const AppState& State() const noexcept;
    [[nodiscard]] const AppConfig& Config() const noexcept;

private:
    enum class Pending { Initialize, Tasks, TasksMore, History, HistoryMore, Categories, Stats, CreateTask, EditTask, Toggle, DeleteTask, Reorder, CreateCategory, UpdateCategory, DeleteCategory, Restore, RestoreBatch, DeleteHistory };
    std::uint64_t Submit(Pending operation, DbCommand command, bool read = false);
    // more=false starts a new pagination session (fresh clock value, no cursor);
    // more=true freezes query.now to state_.taskCursor->sessionNow so the repository signature
    // stays stable across continuation pages. OnCompletions stores page.nextCursor verbatim.
    void LoadTasks(bool more);
    void LoadHistory(bool more);
    void LoadCategories();
    void LoadStats();
    void RefreshAfterWrite();
    void ApplyError(const AppError& error);
    bool CanWrite();
    // --- hot switch internals (design 6.2) ---------------------------------
    // Submits EnsureDatabaseAndSchema() on the candidate worker. Deliberately not routed through
    // Submit(): the probe must not enter pending_, which is keyed by request id and belongs to the
    // current worker - the two workers number their requests independently starting at 1.
    void SubmitProbe();
    void DrainCandidate();
    void AdoptWorker(std::unique_ptr<DbWorker> candidate, const DataSourceCommitResult& committed);
    void FailSwitch(const AppError& error);
    [[nodiscard]] std::string SourceLabel() const;
    AppConfig config_;
    std::unique_ptr<DbWorker> worker_;
    std::shared_ptr<IClock> clock_;
    std::function<void()> changed_;
    Logger& logger_;
    DataSourceSwitchPort port_;
    // Candidate worker + draft while a switch is in flight; both empty otherwise.
    std::unique_ptr<DbWorker> candidate_;
    std::optional<DataSourceDraft> switchDraft_;
    std::uint64_t probeRequest_{0};
    bool readOnlyBeforeSwitch_{true};
    AppState state_;
    std::chrono::steady_clock::time_point lastReconnect_{};
    std::chrono::steady_clock::time_point lastStats_{};
    std::optional<std::string> pendingKeyword_;
    std::optional<std::string> pendingHistoryKeyword_;
    std::chrono::steady_clock::time_point keywordAt_{};
    std::chrono::steady_clock::time_point historyKeywordAt_{};
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::unordered_map<std::string, TaskPage> taskCache_;
    bool started_{false};
    int reconnectAttempts_{0};
};

}  // namespace todo
