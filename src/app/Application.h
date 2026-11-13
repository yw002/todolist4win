#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "app/AppController.h"
#include "app/Logger.h"
#include "app/Win32UiDispatcher.h"
#include "app/WorkerRecycler.h"
#include "config/AppPaths.h"
#include "config/ConfigStore.h"
#include "config/DpapiCredentialVault.h"
#include "config/SettingsService.h"
#include "ui/MainWindow.h"

namespace todo {

// Composition root (design 6.1). Owns the objects that need a lifetime longer than a single data
// source - the settings stack, the current worker, the retired-worker recycler - and is the only
// place that knows which concrete factory belongs to which DataSourceKind.
class Application {
public:
    explicit Application(HINSTANCE instance);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    int Run();

private:
    // The single place that turns "a data source kind + a database config + a password" into a
    // worker. Both the initial start and every candidate switch go through it, so a MySQL password
    // can only ever come from SettingsService - never from an AppConfig field.
    [[nodiscard]] std::unique_ptr<DbWorker> CreateWorker(DataSourceKind kind, const DatabaseConfig& database, const std::string& mysqlPassword);
    // Called exactly once per start (design 2.4). Returns the message to show, or an empty string
    // when there is nothing to say. A failure is deliberately not retried: the plaintext line stays
    // in the INI, the vault is untouched, and the user gets one hint instead of a toast loop.
    [[nodiscard]] std::string MigrateLegacyCredentialAtStartup();
    // DataSourceSwitchPort implementation handed to AppController.
    [[nodiscard]] Result<std::unique_ptr<DbWorker>> CreateCandidateWorker(const DataSourceDraft& draft);
    [[nodiscard]] Result<DataSourceCommitResult> CommitDataSource(const DataSourceDraft& draft);

    ConfigStore configStore_;
    AppPaths paths_;
    DpapiCredentialVault vault_;
    SettingsService settings_;
    AppConfig config_;
    Logger logger_;
    MainWindow window_;
    Win32UiDispatcher dispatcher_;
    WorkerRecycler recycler_;
    std::unique_ptr<AppController> controller_;
};

}  // namespace todo
