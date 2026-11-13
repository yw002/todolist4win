#pragma once

#include <cstdint>
#include <string>

#include "domain/Models.h"

namespace todo {

// Which backing store the application is currently using. Local is the default for a fresh
// install; MySql is selected either explicitly (INI [data] source=mysql) or by the legacy-config
// upgrade path in ConfigStore. The data layer itself only sees an IUnitOfWorkFactory, so this
// enum never leaks into repositories or the domain.
enum class DataSourceKind { Local, MySql };

struct DatabaseConfig {
    std::string host{"localhost"};
    std::uint16_t port{3306};
    std::string username{"root"};
    // Runtime-only field: it is never written to config.ini. The factory value below is the PRD
    // default used for the in-memory MySQL form; the durable copy lives in the DPAPI credential
    // file (see DpapiCredentialVault) and is loaded by SettingsService.
    std::string password{"123456"};
    std::string database{"todolist_db"};
    int connectTimeoutSeconds{5};
};

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

// How the settings layer should treat the MySQL password that the user left in the dialog.
//
// The three states exist so that "empty input" never has to be interpreted implicitly: the dialog
// reports what the user did, and the resolution of the effective password happens in exactly one
// place (SettingsService). Neither the UI nor the credential vault may invent a fourth meaning.
enum class PasswordAction {
    Keep,     // keep the password that is already stored (or the one already in memory)
    Replace,  // the user typed a new password; newPassword is authoritative, empty means empty
    Clear     // the user asked to delete the saved credential
};

// What the settings dialog hands over: a data source choice plus MySQL fields. The password itself
// is never pre-filled into this structure, and newPassword lives only long enough for
// SettingsService to hand it to the credential vault; nothing here is persisted as plaintext.
struct DataSourceDraft {
    DataSourceKind kind{DataSourceKind::Local};
    DatabaseConfig database;
    PasswordAction passwordAction{PasswordAction::Keep};
    std::string newPassword;
};

struct AppConfig {
    DataSourceKind dataSource{DataSourceKind::Local};
    DatabaseConfig database;
    WindowConfig window;
    AppearanceConfig appearance;

    void Sanitize() noexcept;
};

}  // namespace todo
