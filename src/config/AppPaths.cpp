#include "config/AppPaths.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstddef>
#include <system_error>

namespace todo {
namespace {

// Resolve one known folder. SHGetKnownFolderPath is preferred; when the shell cannot answer
// (e.g. a stripped-down test host without COM) the documented %APPDATA%/%LOCALAPPDATA% variables
// are a faithful equivalent. allowWorkingDirectory is only set for the roaming/config root so the
// historical "%APPDATA% missing -> current_path()\TodoListWin" behaviour is not regressed; local
// data must never land in the working directory.
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id, const wchar_t* envName, bool allowWorkingDirectory) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        std::filesystem::path resolved(raw);
        CoTaskMemFree(raw);
        if (!resolved.empty()) return resolved;
    } else if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(envName, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
    return allowWorkingDirectory ? std::filesystem::current_path() : std::filesystem::path{};
}

Result<void> EnsureDirectory(const std::filesystem::path& path, const char* failureMessage) {
    if (path.empty()) return Result<void>::Failure({ErrorCode::Internal, failureMessage});
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error && !std::filesystem::is_directory(path)) {
        return Result<void>::Failure({ErrorCode::PermissionDenied, failureMessage});
    }
    return Result<void>::Success();
}

}  // namespace

AppPaths AppPaths::Default() {
    AppPaths paths;
    const auto roaming = KnownFolder(FOLDERID_RoamingAppData, L"APPDATA", true);
    const auto local = KnownFolder(FOLDERID_LocalAppData, L"LOCALAPPDATA", false);
    paths.roamingRoot = roaming / L"TodoListWin";
    if (!local.empty()) paths.localRoot = local / L"TodoListWin";
    paths.resolved = !paths.localRoot.empty();
    return paths;
}

AppPaths AppPaths::ForRoots(std::filesystem::path roamingRoot, std::filesystem::path localRoot) {
    AppPaths paths;
    paths.roamingRoot = std::move(roamingRoot);
    paths.localRoot = std::move(localRoot);
    paths.resolved = !paths.roamingRoot.empty() && !paths.localRoot.empty();
    return paths;
}

std::filesystem::path AppPaths::ConfigPath() const { return roamingRoot / L"config.ini"; }
std::filesystem::path AppPaths::AppNamePath() const { return roamingRoot / L"app-name.txt"; }
std::filesystem::path AppPaths::LocalDataDirectory() const { return localRoot / L"data"; }
std::filesystem::path AppPaths::LocalDatabasePath() const { return LocalDataDirectory() / L"todolist.db"; }
std::filesystem::path AppPaths::LogDirectory() const { return localRoot / L"Logs"; }
std::filesystem::path AppPaths::LogPath() const { return LogDirectory() / L"todolist.log"; }

Result<void> AppPaths::EnsureLocalDataDirectory() const {
    return EnsureDirectory(LocalDataDirectory(), "无法创建本地数据目录，请检查 %LOCALAPPDATA% 权限");
}

}  // namespace todo
