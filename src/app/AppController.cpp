#include "app/AppController.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/CategoryService.h"
#include "core/StatisticsService.h"
#include "domain/TaskPagination.h"

namespace todo {

AppController::AppController(AppConfig config, std::unique_ptr<DbWorker> worker, std::shared_ptr<IClock> clock, Logger& logger, std::function<void()> changed)
    : config_(std::move(config)), worker_(std::move(worker)), clock_(std::move(clock)), changed_(std::move(changed)), logger_(logger) {
    state_.topmost = config_.window.topmost;
    state_.closeToTray = config_.window.closeToTray;
    state_.completionDisplay = config_.appearance.completionDisplay;
    state_.themeMode = config_.appearance.themeMode;
}

AppController::~AppController() {
    if (worker_) worker_->Stop();
}

void AppController::Start() { if (started_) return; started_ = true; worker_->Start(); Refresh(); }

std::uint64_t AppController::Submit(Pending operation, DbCommand command, bool read) {
    // Submit runs on the UI thread and the worker notifies through PostMessage, which is always
    // asynchronous, so a rejected request can never deliver its completion before pending_ is filled.
    const auto id = worker_->Submit(state_.generation, std::move(command), read);
    pending_[id] = operation;
    logger_.Info("db_command", id);
    return id;
}

void AppController::Dispatch(const AppIntent& intent) {
    std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NavigateIntent>) Navigate(value.route, value.categoryId);
        else if constexpr (std::is_same_v<T, KeywordIntent>) SetKeyword(value.keyword);
        else if constexpr (std::is_same_v<T, HistoryKeywordIntent>) SetHistoryKeyword(value.keyword);
        else if constexpr (std::is_same_v<T, CreateTaskCommand>) CreateTask(value);
        else if constexpr (std::is_same_v<T, EditTaskCommand>) EditTask(value);
        else if constexpr (std::is_same_v<T, ToggleCompletionIntent>) SetCompleted(value.taskId, value.completed, value.revision);
        else if constexpr (std::is_same_v<T, DeleteTaskIntent>) DeleteTask(value.taskId);
        else if constexpr (std::is_same_v<T, RestoreTaskIntent>) Restore(value.taskId);
        else if constexpr (std::is_same_v<T, DeleteHistoryIntent>) DeleteHistory(value.eventIds);
        else if constexpr (std::is_same_v<T, ReorderRequest>) Reorder(value);
    }, intent);
}

void AppController::Navigate(Route route, std::optional<std::int64_t> categoryId) {
    // TaskPage's optional<TaskCursor> is copied whole, so cached views keep the frozen sessionNow.
    if (state_.route != Route::History && state_.route != Route::Statistics) taskCache_[std::to_string(static_cast<int>(state_.route)) + ":" + std::to_string(state_.categoryId.value_or(0))] = TaskPage{state_.tasks, state_.taskCursor, ""};
    state_.route = route;
    state_.categoryId = categoryId;
    ++state_.generation;
    state_.errorMessage.clear();
    worker_->CancelPendingReads();
    const std::string key = std::to_string(static_cast<int>(route)) + ":" + std::to_string(categoryId.value_or(0));
    if (auto it = taskCache_.find(key); it != taskCache_.end() && (route == Route::AllTasks || route == Route::Today || route == Route::Overdue || route == Route::Category)) {
        state_.tasks = it->second.items;
        state_.taskCursor = it->second.nextCursor;
        state_.hasTaskNext = it->second.nextCursor.has_value();
    } else if (route != Route::History && route != Route::Statistics) { state_.tasks.clear(); state_.taskCursor.reset(); state_.hasTaskNext = false; }
    if (route == Route::History) LoadHistory(false);
    else if (route == Route::Statistics) LoadStats();
    else LoadTasks(false);
    changed_();
}

void AppController::SetKeyword(const std::string& keyword) { pendingKeyword_ = keyword; keywordAt_ = std::chrono::steady_clock::now(); changed_(); }
void AppController::SetHistoryKeyword(const std::string& keyword) { pendingHistoryKeyword_ = keyword; historyKeywordAt_ = std::chrono::steady_clock::now(); changed_(); }

bool AppController::FlushPendingSearch(bool force) {
    const auto now = std::chrono::steady_clock::now();
    bool pending = false;
    if (pendingKeyword_ && (force || now - keywordAt_ >= std::chrono::milliseconds(250))) {
        state_.filter.keyword = *pendingKeyword_;
        pendingKeyword_.reset();
        ++state_.generation;
        worker_->CancelPendingReads();
        LoadTasks(false);
        changed_();
    } else pending = pendingKeyword_.has_value();
    if (pendingHistoryKeyword_ && (force || now - historyKeywordAt_ >= std::chrono::milliseconds(250))) {
        state_.historyFilter.keyword = *pendingHistoryKeyword_;
        pendingHistoryKeyword_.reset();
        ++state_.generation;
        worker_->CancelPendingReads();
        LoadHistory(false);
        changed_();
    } else pending = pending || pendingHistoryKeyword_.has_value();
    return pending;
}
void AppController::SetCompletionDisplay(CompletionDisplay display) { state_.completionDisplay = display; config_.appearance.completionDisplay = display; ++state_.generation; LoadTasks(false); changed_(); }
void AppController::SetTopmost(bool value) noexcept { state_.topmost = value; config_.window.topmost = value; changed_(); }
void AppController::SetThemeMode(ThemeMode mode) noexcept { state_.themeMode = mode; config_.appearance.themeMode = mode; changed_(); }
void AppController::ClearToast() { state_.toast.clear(); changed_(); }
void AppController::ShowToast(const std::string& message) { state_.toast = message; changed_(); }

void AppController::Tick() {
    if (!started_) return;
    const auto now = std::chrono::steady_clock::now();
    const bool initializing = std::any_of(pending_.begin(), pending_.end(), [](const auto& item) { return item.second == Pending::Initialize; });
    static constexpr int delays[] = {1, 2, 5, 10, 30};
    // Initializing is retried as well: the very first load can be cancelled by a navigation, and
    // leaving the state there would stall the app on "connecting" forever.
    const bool needsConnection = state_.connection == ConnectionState::Degraded || state_.connection == ConnectionState::Initializing || state_.connection == ConnectionState::Reconnecting;
    if (needsConnection && !initializing && now - lastReconnect_ >= std::chrono::seconds(delays[std::min(reconnectAttempts_, 4)])) {
        ++reconnectAttempts_;
        Refresh();
    }
    const bool statsPending = std::any_of(pending_.begin(), pending_.end(), [](const auto& item) { return item.second == Pending::Stats; });
    if (state_.connection == ConnectionState::Online && !statsPending && now - lastStats_ >= std::chrono::seconds(60)) LoadStats();
}

bool AppController::CanWrite() {
    if (state_.readOnly || state_.connection != ConnectionState::Online) { state_.toast = "数据库未连接，当前为只读状态"; changed_(); return false; }
    return true;
}

void AppController::CreateTask(const CreateTaskCommand& command) {
    if (!CanWrite()) return;
    Submit(Pending::CreateTask, [this, command](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock_); auto result = service.Create(command);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::EditTask(const EditTaskCommand& command) {
    if (!CanWrite()) return;
    Submit(Pending::EditTask, [this, command](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock_); auto result = service.Edit(command);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::SetCompleted(std::int64_t id, bool completed, std::uint64_t revision) {
    if (!CanWrite()) return;
    Submit(Pending::Toggle, [this, id, completed, revision](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock_); auto result = service.SetCompleted(id, completed, revision);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::DeleteTask(std::int64_t id) {
    if (!CanWrite()) return;
    Submit(Pending::DeleteTask, [this, id](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock_); auto result = service.Delete(id);
        return result.ok() ? Result<WorkerValue>::Success(WorkerValue{}) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::Reorder(const ReorderRequest& request) {
    if (!CanWrite() || state_.route == Route::Today || state_.route == Route::Overdue) return;
    Submit(Pending::Reorder, [this, request](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock_); auto result = service.Reorder(request);
        return result.ok() ? Result<WorkerValue>::Success(WorkerValue{}) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::CreateCategory(const std::string& name, const std::string& color) {
    if (!CanWrite()) return;
    Submit(Pending::CreateCategory, [this, name, color](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        CategoryService service(factory, clock_); auto result = service.Create(name, color);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::UpdateCategory(std::int64_t id, const std::string& name, const std::string& color) {
    if (!CanWrite()) return;
    Submit(Pending::UpdateCategory, [this, id, name, color](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        CategoryService service(factory, clock_); auto result = service.Update(id, name, color);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::DeleteCategory(std::int64_t id) {
    if (!CanWrite()) return;
    Submit(Pending::DeleteCategory, [this, id](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        CategoryService service(factory, clock_); auto result = service.Delete(id);
        return result.ok() ? Result<WorkerValue>::Success(WorkerValue{}) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::Restore(std::int64_t id) {
    if (!CanWrite()) return;
    Submit(Pending::Restore, [this, id](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        HistoryService service(factory, clock_); auto result = service.Restore(id);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    });
}
void AppController::RestoreBatch(const std::vector<std::int64_t>& ids) {
    if (!CanWrite() || ids.empty()) return;
    Submit(Pending::RestoreBatch, [this, ids](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        HistoryService service(factory, clock_); return Result<WorkerValue>::Success(service.RestoreBatch(ids));
    });
}
void AppController::DeleteHistory(const std::vector<std::int64_t>& ids) {
    if (!CanWrite() || ids.empty()) return;
    Submit(Pending::DeleteHistory, [this, ids](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        HistoryService service(factory, clock_); auto result = service.DeleteEvents(ids);
        return result.ok() ? Result<WorkerValue>::Success(WorkerValue{}) : Result<WorkerValue>::Failure(result.error());
    });
}

void AppController::LoadTasks(bool more) {
    if (state_.route == Route::History || state_.route == Route::Statistics) return;
    TaskQuery query;
    query.route = state_.route;
    query.categoryId = state_.categoryId;
    query.filter = state_.filter;
    query.display = state_.completionDisplay;
    query.page.pageSize = 200;
    // Take the clock exactly once. A continuation page must resolve to the time frozen in the
    // cursor; using a fresh clock value here would change the Overdue signature and break paging.
    const LocalDateTime freshNow = clock_->NowLocal();
    std::optional<TaskCursor> sessionCursor;
    if (more) sessionCursor = state_.taskCursor;
    const auto resolved = TaskPagination::ResolveTaskQueryNow(more, sessionCursor, freshNow);
    if (!resolved.ok()) {
        // Never submit an old cursor with a new clock value. Drop the broken session and tell the
        // user to start a new first-page session instead of pretending the load succeeded.
        state_.taskCursor.reset();
        state_.hasTaskNext = false;
        ApplyError(resolved.error());
        changed_();
        return;
    }
    if (more) {
        // The resolver above validated that sessionNow exists; never substitute freshNow here.
        query.now = *state_.taskCursor->sessionNow;
        query.page.cursor = state_.taskCursor;
    } else {
        query.now = freshNow;
        query.page.cursor.reset();
        state_.taskCursor.reset();
        state_.hasTaskNext = false;
        // state_.tasks is intentionally kept so a degraded/read-only window can still be browsed.
    }
    state_.loading = true;
    const auto clock = clock_;
    Submit(more ? Pending::TasksMore : Pending::Tasks, [query, clock](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        TaskService service(factory, clock); auto result = service.Query(query);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    }, true);
}
void AppController::LoadHistory(bool more) {
    state_.loading = true;
    HistoryQuery query;
    query.filter = state_.historyFilter;
    query.page.pageSize = 200;
    if (more) query.page.cursor = state_.historyCursor;
    const auto clock = clock_;
    Submit(more ? Pending::HistoryMore : Pending::History, [query, clock](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        HistoryService service(factory, clock); auto result = service.Query(query);
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    }, true);
}
void AppController::LoadCategories() {
    Submit(Pending::Categories, [](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        CategoryService service(factory, std::make_shared<SystemClock>()); auto result = service.List();
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    }, true);
}
void AppController::LoadStats() {
    state_.loading = true;
    lastStats_ = std::chrono::steady_clock::now();
    Submit(Pending::Stats, [this](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        StatisticsService service(factory, clock_); auto result = service.Refresh();
        return result.ok() ? Result<WorkerValue>::Success(result.value()) : Result<WorkerValue>::Failure(result.error());
    }, true);
}
void AppController::LoadMore() {
    const bool tasksBusy = std::any_of(pending_.begin(), pending_.end(), [](const auto& item) {
        return item.second == Pending::TasksMore || item.second == Pending::Tasks || item.second == Pending::Initialize;
    });
    const bool historyMorePending = std::any_of(pending_.begin(), pending_.end(), [](const auto& item) { return item.second == Pending::HistoryMore; });
    if (state_.route == Route::History) {
        if (state_.hasHistoryNext && !historyMorePending) LoadHistory(true);
        return;
    }
    if (!state_.hasTaskNext || tasksBusy) return;
    // A continuation without a frozen session time must not fall back to a fresh clock value.
    if (!state_.taskCursor || !state_.taskCursor->sessionNow) {
        state_.taskCursor.reset();
        state_.hasTaskNext = false;
        return;
    }
    LoadTasks(true);
}
void AppController::LoadBodyImages(std::int64_t taskId, const std::vector<std::string>& keys) {
    // Opening a new editor must never show the previous task's blobs, not even for the short window
    // before the worker answers.
    state_.editorImages.clear();
    state_.editorImageError.clear();
    if (keys.empty()) { changed_(); return; }
    const auto requestId = Submit(Pending::LoadBodyImages, [taskId, keys](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        // Read only: no Begin/Commit. This is the same shape as TaskService::Query.
        auto unit = factory.Create();
        if (!unit.ok()) return Result<WorkerValue>::Failure(unit.error());
        std::vector<BodyImage> images;
        images.reserve(keys.size());
        for (const auto& key : keys) {
            auto image = unit.value()->Images().Get(taskId, key);
            if (image.ok()) { images.push_back(std::move(image.value())); continue; }
            // A blob missing from the local database is a normal state: skip the key and keep
            // collecting. Only connection/query-level errors fail the whole batch.
            if (image.error().code == ErrorCode::ImageUnavailable) continue;
            return Result<WorkerValue>::Failure(image.error());
        }
        return Result<WorkerValue>::Success(WorkerValue{std::move(images)});
    }, true);
    // Submit() only queues: completions are drained asynchronously, so this key is always in place
    // before OnCompletions() can see the result.
    pendingImageKeys_[requestId] = keys;
}
void AppController::Refresh() {
    lastReconnect_ = std::chrono::steady_clock::now();
    ++state_.generation;
    state_.loading = true;
    TaskQuery taskQuery;
    taskQuery.route = state_.route;
    taskQuery.categoryId = state_.categoryId;
    taskQuery.filter = state_.filter;
    taskQuery.display = state_.completionDisplay;
    taskQuery.now = clock_->NowLocal();
    taskQuery.page.pageSize = 200;
    // Initialize is always a fresh first page: no cursor and therefore a new pagination session.
    taskQuery.page.cursor.reset();
    HistoryQuery historyQuery;
    historyQuery.filter = state_.historyFilter;
    historyQuery.page.pageSize = 200;
    const auto clock = clock_;
    Submit(Pending::Initialize, [taskQuery, historyQuery, clock](IUnitOfWorkFactory& factory) -> Result<WorkerValue> {
        auto schema = factory.EnsureDatabaseAndSchema();
        if (!schema.ok()) return Result<WorkerValue>::Failure(schema.error());
        WorkspaceSnapshot data;
        TaskService tasks(factory, clock);
        auto taskPage = tasks.Query(taskQuery); if (!taskPage.ok()) return Result<WorkerValue>::Failure(taskPage.error()); data.tasks = taskPage.value();
        CategoryService categories(factory, clock); auto categoryList = categories.List(); if (!categoryList.ok()) return Result<WorkerValue>::Failure(categoryList.error()); data.categories = categoryList.value();
        StatisticsService statistics(factory, clock); auto stats = statistics.Refresh(); if (!stats.ok()) return Result<WorkerValue>::Failure(stats.error()); data.stats = stats.value();
        HistoryService history(factory, clock); auto historyPage = history.Query(historyQuery); if (!historyPage.ok()) return Result<WorkerValue>::Failure(historyPage.error()); data.history = historyPage.value();
        return Result<WorkerValue>::Success(std::move(data));
    }, true);
}
void AppController::RefreshAfterWrite() { ++state_.dataVersion; ++state_.generation; LoadCategories(); LoadStats(); if (state_.route == Route::History) LoadHistory(false); else if (state_.route != Route::Statistics) LoadTasks(false); }

void AppController::Retry() {
    // Retrying reuses the worker and the SQLite factory that are already open, so no credentials or
    // connection strings are involved.
    if (!worker_) return;
    reconnectAttempts_ = 0;
    state_.errorMessage.clear();
    state_.connection = ConnectionState::Reconnecting;
    state_.readOnly = true;
    Refresh();
    changed_();
}

void AppController::ApplyError(const AppError& error) {
    state_.errorMessage = error.message;
    state_.toast = error.message;
    state_.loading = false;
    if (error.IsConnectionError() || error.code == ErrorCode::PermissionDenied || error.code == ErrorCode::SchemaUnsupported) {
        state_.connection = ErrorCode::SchemaUnsupported == error.code ? ConnectionState::Error : ConnectionState::Degraded;
        state_.readOnly = true;
        reconnectAttempts_ = 0;
    }
}

void AppController::OnCompletions() {
    if (!worker_) return;
    for (auto& completion : worker_->DrainCompletions()) {
        auto pending = pending_.find(completion.requestId);
        if (pending == pending_.end()) continue;
        const Pending operation = pending->second;
        pending_.erase(pending);
        // Cancelled is pure bookkeeping: drop it before the generation check so an old-generation
        // cancelled read still clears loading, and never let it touch state or trigger a reconnect.
        if (!completion.result.ok() && completion.result.error().code == ErrorCode::Cancelled) {
            if (operation == Pending::LoadBodyImages) pendingImageKeys_.erase(completion.requestId);
            state_.loading = false;
            continue;
        }
        const bool write = operation == Pending::CreateTask || operation == Pending::EditTask || operation == Pending::Toggle || operation == Pending::DeleteTask || operation == Pending::Reorder || operation == Pending::CreateCategory || operation == Pending::UpdateCategory || operation == Pending::DeleteCategory || operation == Pending::Restore || operation == Pending::RestoreBatch || operation == Pending::DeleteHistory;
        if (!write && completion.generation != state_.generation) {
            if (operation == Pending::LoadBodyImages) pendingImageKeys_.erase(completion.requestId);
            continue;
        }
        if (!completion.result.ok()) {
            logger_.Error("db_command", completion.result.error(), completion.requestId);
            if (operation == Pending::LoadBodyImages) {
                // Explicit generation guard for the editor path: a slow blob load from an older
                // generation must never paint into the editor that is open now.
                if (completion.generation != state_.generation) { pendingImageKeys_.erase(completion.requestId); continue; }
                pendingImageKeys_.erase(completion.requestId);
                state_.editorImages.clear();
                state_.editorImageError = completion.result.error().message;
                continue;
            }
            ApplyError(completion.result.error());
            continue;
        }
        state_.errorMessage.clear();
        const auto& value = completion.result.value();
        if (operation == Pending::Initialize) {
            const auto& data = std::get<WorkspaceSnapshot>(value);
            state_.tasks = data.tasks.items; state_.taskCursor = data.tasks.nextCursor; state_.hasTaskNext = data.tasks.nextCursor.has_value();
            state_.history = data.history.items; state_.historyCursor = data.history.nextCursor; state_.hasHistoryNext = data.history.nextCursor.has_value();
            state_.categories = data.categories; state_.stats = data.stats;
            state_.connection = ConnectionState::Online; state_.readOnly = false; state_.loading = false; state_.toast = "本地数据库已连接";
            reconnectAttempts_ = 0;
            ++state_.dataVersion;
        } else if (operation == Pending::Tasks || operation == Pending::TasksMore) {
            const auto& page = std::get<TaskPage>(value);
            if (operation == Pending::TasksMore) state_.tasks.insert(state_.tasks.end(), page.items.begin(), page.items.end()); else state_.tasks = page.items;
            // Store page.nextCursor verbatim: it already carries the frozen sessionNow. Never
            // rebuild a cursor from SystemClock here or the next page would break its signature.
            state_.taskCursor = page.nextCursor; state_.hasTaskNext = page.nextCursor.has_value(); state_.loading = false;
        } else if (operation == Pending::History || operation == Pending::HistoryMore) {
            const auto& page = std::get<HistoryPage>(value);
            if (operation == Pending::HistoryMore) state_.history.insert(state_.history.end(), page.items.begin(), page.items.end()); else state_.history = page.items;
            state_.historyCursor = page.nextCursor; state_.hasHistoryNext = page.nextCursor.has_value(); state_.loading = false;
        } else if (operation == Pending::Categories) state_.categories = std::get<std::vector<CategoryCount>>(value);
        else if (operation == Pending::Stats) { state_.stats = std::get<StatsSnapshot>(value); state_.loading = false; ++state_.dataVersion; }
        else if (operation == Pending::LoadBodyImages) {
            // Explicit generation guard for the editor path; the generic read guard above already
            // filters stale results, but this makes the invariant local and obvious.
            if (completion.generation != state_.generation) { pendingImageKeys_.erase(completion.requestId); continue; }
            const auto& images = std::get<std::vector<BodyImage>>(value);
            state_.editorImages = images;
            auto requested = pendingImageKeys_.find(completion.requestId);
            std::size_t missing = 0;
            if (requested != pendingImageKeys_.end()) {
                for (const auto& key : requested->second) {
                    if (std::none_of(images.begin(), images.end(), [&key](const BodyImage& item) { return item.key == key; })) ++missing;
                }
                pendingImageKeys_.erase(requested);
            }
            // The summary is a std::string, not an ErrorCode: a missing blob is a normal state and
            // must not be mistaken for a connection failure or a lost task row.
            state_.editorImageError = missing ? "有 " + std::to_string(missing) + " 张图片在本地数据库中不可用" : std::string();
        }
        else if (operation == Pending::RestoreBatch) {
            state_.lastBatch = std::get<BatchResult>(value); state_.toast = "批量恢复成功 " + std::to_string(state_.lastBatch.Succeeded()) + " 项，失败 " + std::to_string(state_.lastBatch.Failed()) + " 项";
            ++state_.lastSuccessfulWrite; RefreshAfterWrite();
        } else if (write) {
            state_.toast = operation == Pending::DeleteTask ? "任务已删除" : operation == Pending::DeleteHistory ? "历史事件已永久删除" : operation == Pending::DeleteCategory ? "分类已删除" : operation == Pending::Restore ? "任务已恢复" : "已保存";
            ++state_.lastSuccessfulWrite;
            RefreshAfterWrite();
        }
    }
    changed_();
}

AppState& AppController::State() noexcept { return state_; }
const AppState& AppController::State() const noexcept { return state_; }
const AppConfig& AppController::Config() const noexcept { return config_; }

}  // namespace todo
