#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "config/AppConfig.h"
#include "domain/BodyDocument.h"
#include "domain/Models.h"

namespace todo {

enum class FocusedControl { None, NewTitle, DueDate, Search, TaskList, HistorySearch, HistoryStart, HistoryEnd, Dialog };

struct AppState {
    Route route{Route::AllTasks};
    std::optional<std::int64_t> categoryId;
    ConnectionState connection{ConnectionState::Initializing};
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
    // PNG bytes for the task currently open in the editor. CPU-side bytes are the only truth; the
    // editor's ID2D1Bitmaps are derived views that can be rebuilt after a device loss. The error
    // string is a human-readable summary (missing blobs / query failure), never an ErrorCode.
    std::vector<BodyImage> editorImages;
    std::string editorImageError;
    std::string errorMessage;
    std::string toast;
    BatchResult lastBatch;
    bool topmost{false};
    bool closeToTray{true};
    CompletionDisplay completionDisplay{CompletionDisplay::KeepVisible};
    ThemeMode themeMode{ThemeMode::System};
};

}  // namespace todo
