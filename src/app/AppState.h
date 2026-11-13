#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "config/AppConfig.h"
#include "domain/Models.h"

namespace todo {

enum class FocusedControl { None, NewTitle, DueDate, Search, TaskList, HistorySearch, HistoryStart, HistoryEnd, Dialog };

struct AppState {
    Route route{Route::AllTasks};
    std::optional<std::int64_t> categoryId;
    ConnectionState connection{ConnectionState::Initializing};
    // Which store the visible data came from. Mirrors AppController::Config().dataSource so pages
    // and the status bar never have to reach for ConfigStore (design section 7).
    DataSourceKind dataSource{DataSourceKind::Local};
    // True from the moment a switch is requested until the new source has produced its first
    // successful load. While set, every write entry point is closed and the status bar says so.
    bool switching{false};
    // Whether a MySQL password is stored for this user. The UI only ever shows "saved / not saved /
    // cannot be decrypted"; the value itself never leaves SettingsService.
    bool hasSavedCredential{false};
    Overlay overlay{Overlay::None};
    FocusedControl focusedControl{FocusedControl::None};
    TaskFilter filter;
    HistoryFilter historyFilter;
    std::uint64_t generation{1};
    std::uint64_t dataVersion{0};
    std::uint64_t lastSuccessfulWrite{0};
    bool readOnly{true};
    bool loading{false};
    bool hasTaskNext{false};
    bool hasHistoryNext{false};
    std::optional<TaskCursor> taskCursor;
    std::optional<HistoryCursor> historyCursor;
    std::vector<Task> tasks;
    std::vector<HistoryEvent> history;
    std::vector<CategoryCount> categories;
    StatsSnapshot stats;
    std::string errorMessage;
    std::string toast;
    BatchResult lastBatch;
    bool topmost{false};
    bool closeToTray{true};
    CompletionDisplay completionDisplay{CompletionDisplay::KeepVisible};
    ThemeMode themeMode{ThemeMode::System};
};

}  // namespace todo
