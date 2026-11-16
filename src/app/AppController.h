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

class AppController {
public:
    AppController(AppConfig config, std::unique_ptr<DbWorker> worker, std::shared_ptr<IClock> clock, Logger& logger, std::function<void()> changed);
    ~AppController();
    void Start();
    void Dispatch(const AppIntent& intent);
    void OnCompletions();
    // Retries the local database on the worker / factory that is already open. This is what the
    // status-bar retry button calls after the file was temporarily locked or the device failed.
    void Retry();
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
    // Loads the PNG blobs referenced by the open task's body document on the DbWorker thread.
    // Missing single blobs (ImageUnavailable) are skipped, not treated as a batch failure.
    void LoadBodyImages(std::int64_t taskId, const std::vector<std::string>& keys);
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
    enum class Pending { Initialize, Tasks, TasksMore, History, HistoryMore, Categories, Stats, CreateTask, EditTask, Toggle, DeleteTask, Reorder, CreateCategory, UpdateCategory, DeleteCategory, Restore, RestoreBatch, DeleteHistory, LoadBodyImages };
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
    AppConfig config_;
    std::unique_ptr<DbWorker> worker_;
    std::shared_ptr<IClock> clock_;
    std::function<void()> changed_;
    Logger& logger_;
    AppState state_;
    std::chrono::steady_clock::time_point lastReconnect_{};
    std::chrono::steady_clock::time_point lastStats_{};
    std::optional<std::string> pendingKeyword_;
    std::optional<std::string> pendingHistoryKeyword_;
    std::chrono::steady_clock::time_point keywordAt_{};
    std::chrono::steady_clock::time_point historyKeywordAt_{};
    std::unordered_map<std::uint64_t, Pending> pending_;
    // Keys requested by each in-flight LoadBodyImages command. WorkerValue's shape is fixed to
    // std::vector<BodyImage>, so the controller recomputes the "missing blobs" summary by
    // subtracting the returned keys from the requested ones instead of widening the variant.
    std::unordered_map<std::uint64_t, std::vector<std::string>> pendingImageKeys_;
    std::unordered_map<std::string, TaskPage> taskCache_;
    bool started_{false};
    int reconnectAttempts_{0};
};

}  // namespace todo
