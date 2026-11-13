#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "config/ConfigStore.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("ConfigStore", name, value, detail); }

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_test_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" + std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

void WriteRawIni(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::vector<unsigned char> bytes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return bytes;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return bytes;
}

// True when either encoding of the file still contains a "password" key: the writer emits UTF-16LE,
// hand-written legacy files are usually ANSI.
bool RawFileMentionsPassword(const std::filesystem::path& path) {
    const auto bytes = ReadBytes(path);
    const std::string ansi = "password";
    const std::string utf16 = std::string("p\0a\0s\0s\0w\0o\0r\0d\0", 16);
    const auto has = [&bytes](const std::string& needle) {
        return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
    };
    return has(ansi) || has(utf16);
}

bool IniHasPasswordKey(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return false;
    const wchar_t sentinel[] = L"\x1F__absent__\x1F";
    wchar_t buffer[64]{};
    const DWORD length = GetPrivateProfileStringW(L"database", L"password", sentinel, buffer,
                                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
    return !(length == static_cast<DWORD>(std::wcslen(sentinel)) && std::wcsncmp(buffer, sentinel, length) == 0);
}

std::wstring IniValue(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[512]{};
    const DWORD length = GetPrivateProfileStringW(section, key, L"", buffer,
                                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
    return std::wstring(buffer, length);
}
}  // namespace

void RunConfigStoreTests() {
    // --- Round trip of every persisted field --------------------------------
    const auto roundTripRoot = MakeCaseRoot("roundtrip");
    const auto path = roundTripRoot / L"config.ini";
    ConfigStore store(path);
    AppConfig config;
    config.database.host = "127.0.0.1";
    config.database.port = 3307;
    config.database.username = "tester";
    config.database.password = "not-logged-secret";
    config.database.database = "todolist_test_db";
    config.database.connectTimeoutSeconds = 12;
    config.window.topmost = true;
    config.window.closeToTray = false;
    config.window.widthDip = 1234.5;
    config.window.heightDip = 900.25;
    config.appearance.themeMode = ThemeMode::Dark;
    config.appearance.completionDisplay = CompletionDisplay::Archive;

    auto saved = store.Save(config);
    Check("atomic save succeeds", saved.ok());
    Check("config file exists after save", std::filesystem::exists(path));
    Check("temporary file is gone after save", !std::filesystem::exists(roundTripRoot / L"config.ini.tmp"));

    auto loaded = store.Load();
    Check("load succeeds", loaded.ok());
    if (loaded.ok()) {
        const auto& value = loaded.value();
        Check("host roundtrip", value.database.host == "127.0.0.1");
        Check("port roundtrip", value.database.port == 3307);
        Check("username roundtrip", value.database.username == "tester");
        // T13: the password is a runtime-only field. Saving must not persist it, and the value that
        // comes back is the PRD default from DatabaseConfig, not the secret we wrote.
        Check("password is never persisted", value.database.password != "not-logged-secret");
        Check("database roundtrip", value.database.database == "todolist_test_db");
        Check("timeout roundtrip", value.database.connectTimeoutSeconds == 12);
        Check("topmost roundtrip", value.window.topmost);
        Check("close to tray roundtrip", !value.window.closeToTray);
        Check("window size roundtrip", value.window.widthDip == 1234.5 && value.window.heightDip == 900.25);
        Check("theme roundtrip", value.appearance.themeMode == ThemeMode::Dark);
        Check("completion display roundtrip", value.appearance.completionDisplay == CompletionDisplay::Archive);
    }
    Cleanup(roundTripRoot);

    // --- Non-ASCII values must survive the UTF-16 ini round trip ------------
    const auto unicodeRoot = MakeCaseRoot("unicode");
    ConfigStore unicodeStore(unicodeRoot / L"config.ini");
    AppConfig unicode;
    unicode.database.host = "数据库.example";
    unicode.database.username = "测试用户";
    unicode.database.password = "中文值";
    unicode.database.database = "待办库";
    Check("unicode save succeeds", unicodeStore.Save(unicode).ok());
    {
        std::ifstream probe(unicodeRoot / L"config.ini", std::ios::binary);
        unsigned char bom[2]{};
        probe.read(reinterpret_cast<char*>(bom), 2);
        Check("config file starts with UTF-16LE BOM", bom[0] == 0xFF && bom[1] == 0xFE);
    }
    auto unicodeLoaded = unicodeStore.Load();
    Check("unicode load succeeds", unicodeLoaded.ok());
    if (unicodeLoaded.ok()) {
        Check("unicode host roundtrip", unicodeLoaded.value().database.host == "数据库.example");
        Check("unicode username roundtrip", unicodeLoaded.value().database.username == "测试用户");
        Check("unicode password is never persisted", unicodeLoaded.value().database.password != "中文值");
        Check("unicode database roundtrip", unicodeLoaded.value().database.database == "待办库");
    }
    Cleanup(unicodeRoot);

    // --- Missing file means defaults, never a write -------------------------
    const auto missingRoot = MakeCaseRoot("missing");
    ConfigStore missingStore(missingRoot / L"config.ini");
    auto defaults = missingStore.Load();
    Check("missing file loads defaults", defaults.ok());
    if (defaults.ok()) {
        Check("default host", defaults.value().database.host == "localhost");
        Check("default port", defaults.value().database.port == 3306);
        Check("default username follows PRD", defaults.value().database.username == "root");
        // The factory password is deliberately not duplicated in test sources; its value is
        // verified against docs/PRD.md P0-11 by inspection on the config layer.
        Check("default password is configured", !defaults.value().database.password.empty());
        Check("default database follows PRD", defaults.value().database.database == "todolist_db");
        Check("default theme follows system", defaults.value().appearance.themeMode == ThemeMode::System);
        Check("default keeps completed tasks visible", defaults.value().appearance.completionDisplay == CompletionDisplay::KeepVisible);
    }
    Check("load does not create the file", !std::filesystem::exists(missingRoot / L"config.ini"));
    Cleanup(missingRoot);

    // --- Sanitize guards impossible geometry and ports ----------------------
    const auto clampRoot = MakeCaseRoot("clamp");
    ConfigStore clampStore(clampRoot / L"config.ini");
    AppConfig tiny;
    tiny.window.widthDip = 100.0;
    tiny.window.heightDip = 10.0;
    tiny.database.port = 0;
    tiny.database.connectTimeoutSeconds = 600;
    Check("clamping save succeeds", clampStore.Save(tiny).ok());
    auto clamped = clampStore.Load();
    Check("clamping load succeeds", clamped.ok());
    if (clamped.ok()) {
        Check("minimum width enforced", clamped.value().window.widthDip == 760.0);
        Check("minimum height enforced", clamped.value().window.heightDip == 560.0);
        Check("zero port replaced", clamped.value().database.port == 3306);
        Check("timeout clamped to 60", clamped.value().database.connectTimeoutSeconds == 60);
    }
    Cleanup(clampRoot);

    // --- Corrupted values fall back instead of failing ----------------------
    const auto corruptRoot = MakeCaseRoot("corrupt");
    const auto corruptPath = corruptRoot / L"config.ini";
    WriteRawIni(corruptPath,
                "[database]\nhost=\nport=abc\nusername=\ndatabase=\n\n"
                "[window]\ntopmost=yes\nlast_dpi=0\nwidth_dip=not-a-number\n\n"
                "[appearance]\ntheme_mode=purple\ncompletion_display=whatever\n");
    ConfigStore corruptStore(corruptPath);
    auto recovered = corruptStore.Load();
    Check("corrupted file still loads", recovered.ok());
    if (recovered.ok()) {
        Check("invalid port falls back", recovered.value().database.port == 3306);
        Check("empty host falls back", recovered.value().database.host == "localhost");
        Check("empty username falls back", recovered.value().database.username == "root");
        Check("empty database falls back", recovered.value().database.database == "todolist_db");
        Check("unknown theme falls back to system", recovered.value().appearance.themeMode == ThemeMode::System);
        Check("unknown display falls back to keep", recovered.value().appearance.completionDisplay == CompletionDisplay::KeepVisible);
        Check("invalid width falls back", recovered.value().window.widthDip == 1180.0);
        Check("invalid dpi falls back", recovered.value().window.lastDpi == 96);
    }
    Cleanup(corruptRoot);

    // --- Line breaks in stored values must not corrupt the ini --------------
    // The password can no longer be used as the probe value (it is not persisted any more), so the
    // same single-line grammar rule is exercised through the username.
    const auto escapeRoot = MakeCaseRoot("escape");
    ConfigStore escapeStore(escapeRoot / L"config.ini");
    AppConfig multiline;
    multiline.database.username = "line1\nline2";
    Check("save with line break succeeds", escapeStore.Save(multiline).ok());
    auto escaped = escapeStore.Load();
    Check("line break is flattened on reload", escaped.ok() && escaped.value().database.username == "line1 line2");
    Cleanup(escapeRoot);

    // --- [data] source and the legacy upgrade rule (design 2.3) -------------
    const auto sourceRoot = MakeCaseRoot("source");
    const auto sourcePath = sourceRoot / L"config.ini";
    {
        std::error_code error;
        std::filesystem::remove(sourcePath, error);
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("missing file defaults to Local", loaded.ok() && loaded.value().dataSource == DataSourceKind::Local);
    }
    {
        WriteRawIni(sourcePath, "[data]\nsource=mysql\n\n[database]\nhost=db.example\nport=3306\n");
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("explicit source=mysql is honored", loaded.ok() && loaded.value().dataSource == DataSourceKind::MySql);
    }
    {
        WriteRawIni(sourcePath, "[database]\nhost=localhost\nport=3306\nusername=root\n");
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("legacy [database] keys keep MySQL", loaded.ok() && loaded.value().dataSource == DataSourceKind::MySql);
    }
    {
        WriteRawIni(sourcePath, "[data]\nsource=local\n\n[database]\nhost=localhost\npassword=legacy\n");
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("explicit source=local beats legacy database keys", loaded.ok() && loaded.value().dataSource == DataSourceKind::Local);
    }
    {
        WriteRawIni(sourcePath, "[data]\nsource=bogus\n\n[database]\nhost=localhost\n");
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("invalid source falls back to legacy MySQL", loaded.ok() && loaded.value().dataSource == DataSourceKind::MySql);
    }
    {
        WriteRawIni(sourcePath, "[data]\nsource=bogus\n");
        ConfigStore store(sourcePath);
        auto loaded = store.Load();
        Check("invalid source without legacy keys defaults to Local", loaded.ok() && loaded.value().dataSource == DataSourceKind::Local);
    }
    {
        std::error_code error;
        std::filesystem::remove(sourcePath, error);
        ConfigStore store(sourcePath);
        AppConfig config;
        config.dataSource = DataSourceKind::MySql;
        Check("save with mysql source succeeds", store.Save(config).ok());
        auto loaded = store.Load();
        Check("saved source roundtrips as MySql", loaded.ok() && loaded.value().dataSource == DataSourceKind::MySql);
    }
    Cleanup(sourceRoot);

    // --- T13: the password never reaches the file, and an old plaintext line
    //     blocks rewrites until the encrypted copy exists --------------------
    const auto secretRoot = MakeCaseRoot("secret");
    const auto secretPath = secretRoot / L"config.ini";
    {
        ConfigStore store(secretPath);
        AppConfig config;
        config.database.password = "must-not-appear";
        Check("save with a runtime password succeeds", store.Save(config).ok());
        Check("no password key is written", !IniHasPasswordKey(secretPath));
        Check("no password bytes exist anywhere in the file", !RawFileMentionsPassword(secretPath));
        auto reloaded = store.Load();
        Check("a clean file reports no legacy plaintext", reloaded.ok() && !store.HasLegacyPlaintextPassword());
        Check("a clean file has nothing retained", !store.LegacyPasswordRetained());
        Check("nothing can be taken from a clean file", !store.TakeLegacyPlaintextPassword().has_value());
    }
    {
        // A file written by the previous version still carries the plaintext.
        WriteRawIni(secretPath, "[database]\nhost=localhost\nport=3306\npassword=legacy-plain\n");
        ConfigStore store(secretPath);
        auto loaded = store.Load();
        Check("legacy plaintext is detected on load", store.HasLegacyPlaintextPassword());
        Check("legacy plaintext is reported by the retention query", store.LegacyPasswordRetained());
        const auto legacyPlain = store.TakeLegacyPlaintextPassword();
        Check("legacy plaintext is readable for migration",
              legacyPlain.has_value() && *legacyPlain == "legacy-plain");
        // T14: a rewrite must neither destroy the only copy of the secret nor fail (a failing save
        // would break every window-position write and pop a toast on each drag).
        const auto pendingSave = store.Save(loaded.value());
        Check("saving while the plaintext is pending succeeds", pendingSave.ok());
        Check("the pending save rewrote the file", IniHasPasswordKey(secretPath));
        Check("the pending save retained the plaintext line verbatim",
              IniValue(secretPath, L"database", L"password") == L"legacy-plain");
        Check("the store still reports the pending plaintext",
              store.HasLegacyPlaintextPassword() && store.LegacyPasswordRetained());
        Check("a second pending save still succeeds", store.Save(loaded.value()).ok());
        Check("the retained line survives repeated saves",
              IniValue(secretPath, L"database", L"password") == L"legacy-plain");
        store.MarkLegacyPlaintextMigrated();
        Check("save succeeds once the migration is marked", store.Save(loaded.value()).ok());
        Check("the marked save deleted the plaintext line", !IniHasPasswordKey(secretPath));
        Check("the marked save left no password bytes", !RawFileMentionsPassword(secretPath));
        Check("the store stops reporting a legacy plaintext",
              !store.HasLegacyPlaintextPassword() && !store.LegacyPasswordRetained());
    }
    Cleanup(secretRoot);
}
