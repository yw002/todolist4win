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

void WriteIni(const std::filesystem::path& path, const wchar_t* text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const unsigned char bom[2] = {0xFF, 0xFE};
    stream.write(reinterpret_cast<const char*>(bom), 2);
    stream.write(reinterpret_cast<const char*>(text), static_cast<std::streamsize>(std::wcslen(text) * sizeof(wchar_t)));
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::vector<unsigned char> bytes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return bytes;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return bytes;
}

std::wstring IniValue(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[512]{};
    const DWORD length = GetPrivateProfileStringW(section, key, L"__absent__", buffer,
                                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
    return std::wstring(buffer, length);
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
}  // namespace

void RunConfigStoreTests() {
    // --- Round trip of every persisted field --------------------------------
    const auto roundTripRoot = MakeCaseRoot("roundtrip");
    const auto path = roundTripRoot / L"config.ini";
    ConfigStore store(path);
    AppConfig config;
    config.window.topmost = true;
    config.window.closeToTray = false;
    config.window.leftDip = 321.5;
    config.window.topDip = 222.25;
    config.window.widthDip = 1234.5;
    config.window.heightDip = 900.25;
    config.window.lastDpi = 144;
    config.window.maximized = true;
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
        Check("topmost roundtrip", value.window.topmost);
        Check("close to tray roundtrip", !value.window.closeToTray);
        Check("window position roundtrip", value.window.leftDip == 321.5 && value.window.topDip == 222.25);
        Check("window size roundtrip", value.window.widthDip == 1234.5 && value.window.heightDip == 900.25);
        Check("dpi roundtrip", value.window.lastDpi == 144);
        Check("maximized roundtrip", value.window.maximized);
        Check("theme roundtrip", value.appearance.themeMode == ThemeMode::Dark);
        Check("completion display roundtrip", value.appearance.completionDisplay == CompletionDisplay::Archive);
    }
    Cleanup(roundTripRoot);

    // --- The file is UTF-16LE with a BOM so the profile API reads it correctly ---
    const auto bomRoot = MakeCaseRoot("bom");
    {
        ConfigStore bomStore(bomRoot / L"config.ini");
        AppConfig value;
        Check("bom save succeeds", bomStore.Save(value).ok());
        const auto bytes = ReadBytes(bomRoot / L"config.ini");
        Check("config file starts with UTF-16LE BOM", bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE);
    }
    Cleanup(bomRoot);

    // --- Missing file means defaults, never a write -------------------------
    const auto missingRoot = MakeCaseRoot("missing");
    ConfigStore missingStore(missingRoot / L"config.ini");
    auto defaults = missingStore.Load();
    Check("missing file loads defaults", defaults.ok());
    if (defaults.ok()) {
        Check("default position", defaults.value().window.leftDip == 120.0 && defaults.value().window.topDip == 100.0);
        Check("default size", defaults.value().window.widthDip == 1180.0 && defaults.value().window.heightDip == 760.0);
        Check("default dpi", defaults.value().window.lastDpi == 96);
        Check("default theme follows system", defaults.value().appearance.themeMode == ThemeMode::System);
        Check("default keeps completed tasks visible", defaults.value().appearance.completionDisplay == CompletionDisplay::KeepVisible);
    }
    Check("load does not create the file", !std::filesystem::exists(missingRoot / L"config.ini"));
    Cleanup(missingRoot);

    // --- Sanitize guards impossible geometry and dpi ------------------------
    const auto clampRoot = MakeCaseRoot("clamp");
    ConfigStore clampStore(clampRoot / L"config.ini");
    AppConfig tiny;
    tiny.window.widthDip = 100.0;
    tiny.window.heightDip = 10.0;
    tiny.window.lastDpi = 0;
    Check("clamping save succeeds", clampStore.Save(tiny).ok());
    auto clamped = clampStore.Load();
    Check("clamping load succeeds", clamped.ok());
    if (clamped.ok()) {
        Check("minimum width enforced", clamped.value().window.widthDip == 760.0);
        Check("minimum height enforced", clamped.value().window.heightDip == 560.0);
        Check("invalid dpi replaced", clamped.value().window.lastDpi == 96);
    }
    Cleanup(clampRoot);

    // --- Corrupted values fall back instead of failing ----------------------
    const auto corruptRoot = MakeCaseRoot("corrupt");
    const auto corruptPath = corruptRoot / L"config.ini";
    WriteIni(corruptPath,
             L"[window]\ntopmost=yes\nlast_dpi=0\nwidth_dip=not-a-number\nheight_dip=-50\n\n"
             L"[appearance]\ntheme_mode=purple\ncompletion_display=whatever\n");
    ConfigStore corruptStore(corruptPath);
    auto recovered = corruptStore.Load();
    Check("corrupted file still loads", recovered.ok());
    if (recovered.ok()) {
        Check("unknown theme falls back to system", recovered.value().appearance.themeMode == ThemeMode::System);
        Check("unknown display falls back to keep", recovered.value().appearance.completionDisplay == CompletionDisplay::KeepVisible);
        Check("invalid width falls back", recovered.value().window.widthDip == 1180.0);
        Check("negative height is clamped", recovered.value().window.heightDip == 560.0);
        Check("invalid dpi falls back", recovered.value().window.lastDpi == 96);
    }
    Cleanup(corruptRoot);

    // --- Old remote-database sections are ignored, and dropped on save ------
    const auto legacyRoot = MakeCaseRoot("legacy");
    const auto legacyPath = legacyRoot / L"config.ini";
    WriteIni(legacyPath,
             L"[data]\nsource=legacy\n\n[database]\nhost=db.example\nport=3306\npassword=legacy-plain\n\n"
             L"[window]\nwidth_dip=900\nheight_dip=700\n\n[appearance]\ntheme_mode=light\n");
    ConfigStore legacyStore(legacyPath);
    auto legacy = legacyStore.Load();
    Check("legacy file still loads", legacy.ok());
    if (legacy.ok()) {
        Check("legacy section does not change geometry", legacy.value().window.widthDip == 900.0 && legacy.value().window.heightDip == 700.0);
        Check("legacy section does not change theme", legacy.value().appearance.themeMode == ThemeMode::Light);
    }
    Check("save succeeds over a legacy file", legacyStore.Save(legacy.value()).ok());
    Check("legacy [database] is removed by save", IniValue(legacyPath, L"database", L"host") == L"__absent__");
    Check("legacy [data] is removed by save", IniValue(legacyPath, L"data", L"source") == L"__absent__");
    Check("legacy password is removed by save", IniValue(legacyPath, L"database", L"password") == L"__absent__");
    Cleanup(legacyRoot);

    // --- SaveUiState remains the UI flush entry point ----------------------
    const auto uiRoot = MakeCaseRoot("uistate");
    {
        ConfigStore uiStore(uiRoot / L"config.ini");
        AppConfig value;
        value.window.widthDip = 980.0;
        value.window.topmost = true;
        Check("SaveUiState works on a missing file", uiStore.SaveUiState(value).ok());
        auto uiLoaded = uiStore.Load();
        Check("SaveUiState writes the window geometry",
              uiLoaded.ok() && uiLoaded.value().window.widthDip == 980.0 && uiLoaded.value().window.topmost);
    }
    Cleanup(uiRoot);
}
