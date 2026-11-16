#pragma once

#include <cstdint>
#include <string>

#include "domain/Models.h"

namespace todo {

struct WindowConfig {
    bool topmost{false};
    bool closeToTray{true};
    double leftDip{120.0};
    double topDip{100.0};
    double widthDip{1180.0};
    double heightDip{760.0};
    unsigned lastDpi{96};
    bool maximized{false};
};

struct AppearanceConfig {
    ThemeMode themeMode{ThemeMode::System};
    CompletionDisplay completionDisplay{CompletionDisplay::KeepVisible};
};

// The application persists UI state only. All task data lives in the local SQLite file
// (AppPaths::LocalDatabasePath); there is no remote database configuration to carry.
struct AppConfig {
    WindowConfig window;
    AppearanceConfig appearance;

    void Sanitize() noexcept;
};

}  // namespace todo
