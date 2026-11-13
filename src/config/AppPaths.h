#pragma once

#include <filesystem>
#include <string>
#include <utility>

#include "common/Result.h"

namespace todo {

// Single source of truth for every path the application touches. Config roams with the user
// profile (%APPDATA%); the SQLite database, its WAL/SHM side files, the DPAPI credential file and
// the log all stay under %LOCALAPPDATA% so they never roam and DPAPI blobs stay usable on the
// machine/user that created them.
//
// Tests build an overridden instance with ForRoots() so they never read or write the real user
// profile. Default() resolves the known folders and falls back to the APPDATA/LOCALAPPDATA
// environment variables when the shell API is unavailable. It never silently falls back to the
// current working directory or Program Files for local data/credentials: those calls fail with a
// PermissionDenied/Internal error instead.
struct AppPaths {
    std::filesystem::path roamingRoot;  // ...\TodoListWin (config)
    std::filesystem::path localRoot;    // ...\TodoListWin (data/credentials/logs)
    bool resolved{false};

    [[nodiscard]] static AppPaths Default();
    [[nodiscard]] static AppPaths ForRoots(std::filesystem::path roamingRoot, std::filesystem::path localRoot);

    [[nodiscard]] std::filesystem::path ConfigPath() const;            // roamingRoot\config.ini
    [[nodiscard]] std::filesystem::path LocalDataDirectory() const;    // localRoot\data
    [[nodiscard]] std::filesystem::path LocalDatabasePath() const;     // localRoot\data\todolist.db
    [[nodiscard]] std::filesystem::path CredentialsDirectory() const;  // localRoot\credentials
    [[nodiscard]] std::filesystem::path CredentialsPath() const;       // localRoot\credentials\mysql.cred
    [[nodiscard]] std::filesystem::path LogDirectory() const;          // localRoot\Logs
    [[nodiscard]] std::filesystem::path LogPath() const;               // localRoot\Logs\todolist.log

    // Creates the directory, mapping a filesystem failure to PermissionDenied with an actionable
    // message. Returns Internal when the local root could not be resolved at all.
    [[nodiscard]] Result<void> EnsureLocalDataDirectory() const;
    [[nodiscard]] Result<void> EnsureCredentialsDirectory() const;
};

}  // namespace todo
