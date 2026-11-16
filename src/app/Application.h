#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "app/AppController.h"
#include "app/Logger.h"
#include "app/Win32UiDispatcher.h"
#include "config/AppPaths.h"
#include "config/ConfigStore.h"
#include "ui/MainWindow.h"

namespace todo {

// Composition root: owns the long-lived objects and builds the single local-database worker.
// The application has exactly one data store now - the SQLite file under %LOCALAPPDATA% - so no
// factory construction and worker lifecycle live here; configuration stays in ConfigStore.
class Application {
public:
    explicit Application(HINSTANCE instance);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    int Run();

private:
    [[nodiscard]] std::unique_ptr<DbWorker> CreateWorker();

    ConfigStore configStore_;
    AppPaths paths_;
    AppConfig config_;
    Logger logger_;
    MainWindow window_;
    Win32UiDispatcher dispatcher_;
    std::unique_ptr<AppController> controller_;
};

}  // namespace todo
