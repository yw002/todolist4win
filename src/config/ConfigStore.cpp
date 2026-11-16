#include "config/ConfigStore.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#include "config/AppPaths.h"

namespace todo {
namespace {
std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string ReadValue(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const std::string& fallback) {
    const std::wstring defaultValue = Utf8ToWide(fallback);
    std::wstring buffer(4096, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, defaultValue.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(length);
    return WideToUtf8(buffer);
}

int ReadInt(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, int fallback) {
    const std::string value = ReadValue(path, section, key, std::to_string(fallback));
    try {
        std::size_t used = 0;
        const int parsed = std::stoi(value, &used);
        return used == value.size() ? parsed : fallback;
    } catch (...) { return fallback; }
}

double ReadDouble(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, double fallback) {
    const std::string value = ReadValue(path, section, key, std::to_string(fallback));
    try {
        std::size_t used = 0;
        const double parsed = std::stod(value, &used);
        return used == value.size() ? parsed : fallback;
    } catch (...) { return fallback; }
}
}  // namespace

ConfigStore::ConfigStore() : path_(DefaultPath()) {}
ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}
const std::filesystem::path& ConfigStore::Path() const noexcept { return path_; }

std::filesystem::path ConfigStore::DefaultPath() { return AppPaths::Default().ConfigPath(); }

Result<AppConfig> ConfigStore::Load() const {
    AppConfig config;
    if (!std::filesystem::exists(path_)) return Result<AppConfig>::Success(config);
    config.window.topmost = ReadInt(path_, L"window", L"topmost", 0) != 0;
    config.window.closeToTray = ReadInt(path_, L"window", L"close_to_tray", 1) != 0;
    config.window.leftDip = ReadDouble(path_, L"window", L"left_dip", config.window.leftDip);
    config.window.topDip = ReadDouble(path_, L"window", L"top_dip", config.window.topDip);
    config.window.widthDip = ReadDouble(path_, L"window", L"width_dip", config.window.widthDip);
    config.window.heightDip = ReadDouble(path_, L"window", L"height_dip", config.window.heightDip);
    config.window.lastDpi = static_cast<unsigned>(ReadInt(path_, L"window", L"last_dpi", 96));
    config.window.maximized = ReadInt(path_, L"window", L"maximized", 0) != 0;
    const std::string theme = ReadValue(path_, L"appearance", L"theme_mode", "system");
    if (theme == "light") config.appearance.themeMode = ThemeMode::Light;
    else if (theme == "dark") config.appearance.themeMode = ThemeMode::Dark;
    else config.appearance.themeMode = ThemeMode::System;
    const std::string display = ReadValue(path_, L"appearance", L"completion_display", "keep");
    config.appearance.completionDisplay = display == "archive" ? CompletionDisplay::Archive : CompletionDisplay::KeepVisible;
    // [data]/[database] from older builds are intentionally ignored: this build is local-only and
    // the next Save() drops those sections from the file.
    config.Sanitize();
    return Result<AppConfig>::Success(config);
}

Result<void> ConfigStore::Save(const AppConfig& config) const {
    AppConfig safe = config;
    safe.Sanitize();
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) return Result<void>::Failure({ErrorCode::Internal, "无法创建配置目录"});
    const auto temporary = path_.parent_path() / (path_.filename().wstring() + L".tmp");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Result<void>::Failure({ErrorCode::Internal, "无法写入配置文件"});
    // GetPrivateProfileStringW reads this file, so it must be UTF-16LE: writing UTF-8 while reading
    // as UTF-16 mangles non-ASCII text. The BOM selects the encoding.
    std::wostringstream content;
    content << L"[window]\n"
            << L"topmost=" << (safe.window.topmost ? 1 : 0) << L"\n"
            << L"close_to_tray=" << (safe.window.closeToTray ? 1 : 0) << L"\n"
            << L"left_dip=" << safe.window.leftDip << L"\n"
            << L"top_dip=" << safe.window.topDip << L"\n"
            << L"width_dip=" << safe.window.widthDip << L"\n"
            << L"height_dip=" << safe.window.heightDip << L"\n"
            << L"last_dpi=" << safe.window.lastDpi << L"\n"
            << L"maximized=" << (safe.window.maximized ? 1 : 0) << L"\n\n"
            << L"[appearance]\n"
            << L"theme_mode=" << (safe.appearance.themeMode == ThemeMode::Light ? L"light" : safe.appearance.themeMode == ThemeMode::Dark ? L"dark" : L"system") << L"\n"
            << L"completion_display=" << (safe.appearance.completionDisplay == CompletionDisplay::Archive ? L"archive" : L"keep") << L"\n";
    static_assert(sizeof(wchar_t) == 2, "The ini file is written as UTF-16LE and requires a 2-byte wchar_t.");
    const std::wstring text = content.str();
    const unsigned char bom[2] = {0xFF, 0xFE};
    stream.write(reinterpret_cast<const char*>(bom), 2);
    stream.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    stream.flush();
    if (!stream) { stream.close(); std::filesystem::remove(temporary, ec); return Result<void>::Failure({ErrorCode::Internal, "配置写入失败"}); }
    stream.close();
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return Result<void>::Failure({ErrorCode::Internal, "无法原子替换配置文件"});
    }
    // The profile API caches file contents; after the atomic replacement the next
    // GetPrivateProfileStringW could still return pre-replacement values. A NULL/NULL/NULL call is
    // the documented cache flush. Its failure is ignored: the file on disk is already correct.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path_.c_str());
    return Result<void>::Success();
}

}  // namespace todo
