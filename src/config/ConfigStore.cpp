#include "config/ConfigStore.h"

#include <windows.h>

#include <cctype>
#include <cwchar>
#include <iterator>
#include <filesystem>
#include <fstream>
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

bool HasProfileKey(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key) {
    // Presence test that also works for an empty value: GetPrivateProfileStringW substitutes the
    // caller-supplied default only when the key is missing, so returning the sentinel means absent.
    const wchar_t sentinel[] = L"\x1F__absent__\x1F";
    wchar_t buffer[64]{};
    const DWORD length = GetPrivateProfileStringW(section, key, sentinel, buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    return !(length == static_cast<DWORD>(std::wcslen(sentinel)) && std::wcsncmp(buffer, sentinel, length) == 0);
}

std::string NormalizeSourceToken(const std::string& value) {
    std::string token;
    token.reserve(value.size());
    for (char c : value) token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = token.find_last_not_of(" \t\r\n");
    return token.substr(first, last - first + 1);
}

std::wstring FlattenValue(const std::string& utf8) {
    // Ini values are single-line: line breaks would break the key/value grammar.
    std::wstring value = Utf8ToWide(utf8);
    for (wchar_t& character : value) {
        if (character == L'\r' || character == L'\n') character = L' ';
    }
    return value;
}
}  // namespace

ConfigStore::ConfigStore() : path_(DefaultPath()) {}
ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}
const std::filesystem::path& ConfigStore::Path() const noexcept { return path_; }

std::filesystem::path ConfigStore::DefaultPath() { return AppPaths::Default().ConfigPath(); }

Result<AppConfig> ConfigStore::Load() const {
    AppConfig config;
    if (!std::filesystem::exists(path_)) return Result<AppConfig>::Success(config);
    config.database.host = ReadValue(path_, L"database", L"host", config.database.host);
    const int port = ReadInt(path_, L"database", L"port", config.database.port);
    config.database.port = port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : config.database.port;
    config.database.username = ReadValue(path_, L"database", L"username", config.database.username);
    config.database.password = ReadValue(path_, L"database", L"password", config.database.password);
    config.database.database = ReadValue(path_, L"database", L"database", config.database.database);
    config.database.connectTimeoutSeconds = ReadInt(path_, L"database", L"connect_timeout_seconds", config.database.connectTimeoutSeconds);
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
    // Data source resolution (design section 2.3): an explicit, valid [data] source wins. If it is
    // missing or invalid, fall back to the legacy-config upgrade rule: any old [database] key means
    // this is a pre-upgrade MySQL install, so it must keep starting on MySQL instead of showing an
    // empty local database. A brand-new install has neither, so it defaults to Local.
    const std::string source = NormalizeSourceToken(ReadValue(path_, L"data", L"source", ""));
    bool haveExplicitSource = false;
    if (source == "local") { config.dataSource = DataSourceKind::Local; haveExplicitSource = true; }
    else if (source == "mysql") { config.dataSource = DataSourceKind::MySql; haveExplicitSource = true; }
    if (!haveExplicitSource) {
        static constexpr const wchar_t* kLegacyKeys[] = {L"host", L"port", L"username", L"password", L"database", L"connect_timeout_seconds"};
        bool legacy = false;
        for (const wchar_t* key : kLegacyKeys) {
            if (HasProfileKey(path_, L"database", key)) { legacy = true; break; }
        }
        config.dataSource = legacy ? DataSourceKind::MySql : DataSourceKind::Local;
    }
    // A plaintext password line can only come from a pre-T13 file. Remember that fact (never the
    // value in logs) so Save() keeps re-emitting that line until the encrypted copy exists.
    legacyPlaintextPresent_ = HasProfileKey(path_, L"database", L"password");
    config.Sanitize();
    return Result<AppConfig>::Success(config);
}

bool ConfigStore::HasLegacyPlaintextPassword() const noexcept { return legacyPlaintextPresent_; }

std::optional<std::string> ConfigStore::TakeLegacyPlaintextPassword() const {
    // Re-read the file instead of caching the secret: the answer must describe the INI as it is on
    // disk right now, not the in-memory copy from a Load() several operations ago.
    if (!std::filesystem::exists(path_) || !HasProfileKey(path_, L"database", L"password")) {
        legacyPlaintextPresent_ = false;
        return std::nullopt;
    }
    legacyPlaintextPresent_ = true;
    return ReadValue(path_, L"database", L"password", "");
}

void ConfigStore::MarkLegacyPlaintextMigrated() noexcept { legacyPlaintextPresent_ = false; }

bool ConfigStore::LegacyPasswordRetained() const noexcept {
    std::error_code ec;
    const bool present = std::filesystem::exists(path_, ec) && !ec && HasProfileKey(path_, L"database", L"password");
    legacyPlaintextPresent_ = present;
    return present;
}

Result<void> ConfigStore::Save(const AppConfig& config) const {
    // Design 2.4 step 4, revised: while the INI still holds a plaintext password, rewriting the file
    // would silently destroy the only copy of the secret. Refusing would be just as bad in practice
    // (MainWindow saves the window position on every close and on every successful connect, so a
    // pending migration would turn each of those into a toast). The file is therefore rewritten
    // normally, and the existing password= line is re-emitted verbatim. This writes no new secret;
    // it preserves state that is already on disk, and SettingsService removes it by migrating.
    //
    // The cached flag alone decides, and the value is read from disk only when it says "retain".
    // Asking LegacyPasswordRetained() here would re-arm the retention immediately after
    // MarkLegacyPlaintextMigrated(), because the line really is still on disk at that moment.
    std::optional<std::string> retainedPlaintext;
    if (legacyPlaintextPresent_) {
        if (HasProfileKey(path_, L"database", L"password")) {
            retainedPlaintext = ReadValue(path_, L"database", L"password", "");
        } else {
            // The line disappeared by other means; nothing to preserve any more.
            legacyPlaintextPresent_ = false;
        }
    }
    AppConfig safe = config;
    safe.Sanitize();
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) return Result<void>::Failure({ErrorCode::Internal, "无法创建配置目录"});
    const auto temporary = path_.parent_path() / (path_.filename().wstring() + L".tmp");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Result<void>::Failure({ErrorCode::Internal, "无法写入配置文件"});
    // The reader is GetPrivateProfileStringW, so the file must be UTF-16LE. Writing UTF-8 while
    // reading as UTF-16 mangles any non-ASCII host/user/password; the BOM selects the encoding.
    std::wostringstream content;
    // [data] source is written first so a reader always knows which store is active before it sees
    // the MySQL fields.
    //
    // There is deliberately no password= line emitted from the config itself (design 2.2 / T13): the
    // MySQL password is only ever persisted through the DPAPI credential file. The only exception is
    // the line retained above, which is copied from the file being replaced. Because the whole file
    // is rewritten from this stream and then atomically replaced, that line disappears on the first
    // save after MarkLegacyPlaintextMigrated(); WritePrivateProfileStringW(..., nullptr) is
    // therefore not needed (and would be an in-place, non-atomic edit anyway).
    content << L"[data]\n"
            << L"source=" << (safe.dataSource == DataSourceKind::MySql ? L"mysql" : L"local") << L"\n\n"
            << L"[database]\n"
            << L"host=" << FlattenValue(safe.database.host) << L"\n"
            << L"port=" << safe.database.port << L"\n"
            << L"username=" << FlattenValue(safe.database.username) << L"\n";
    if (retainedPlaintext.has_value()) content << L"password=" << FlattenValue(*retainedPlaintext) << L"\n";
    content << L"database=" << FlattenValue(safe.database.database) << L"\n"
            << L"connect_timeout_seconds=" << safe.database.connectTimeoutSeconds << L"\n\n"
            << L"[window]\n"
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
    // The Win32 profile API caches file contents; after an external/atomic replacement the next
    // GetPrivateProfileStringW could otherwise still return the pre-replacement values. Calling
    // WritePrivateProfileStringW with all three value arguments NULL is the documented cache flush.
    // Its failure is intentionally ignored: the ini on disk is already correct, so Save succeeded.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path_.c_str());
    // A retained plaintext is still on disk (and still unmigrated), so the flag must stay set: the
    // next Save has to keep re-emitting it too. Only MarkLegacyPlaintextMigrated() clears it.
    legacyPlaintextPresent_ = retainedPlaintext.has_value();
    return Result<void>::Success();
}

}  // namespace todo
