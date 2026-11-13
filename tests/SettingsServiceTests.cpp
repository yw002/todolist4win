#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "FakeCredentialVault.h"
#include "config/AppPaths.h"
#include "config/ConfigStore.h"
#include "config/SettingsService.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") {
    ReportTest("SettingsService", name, value, detail);
}

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_settings_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" +
                             std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

void WriteRawIni(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::vector<unsigned char> bytes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return bytes;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return bytes;
}

// Semantic presence test for the plaintext key: GetPrivateProfileStringW substitutes the caller's
// default only when the key is absent, so a returned sentinel means "no password line".
bool IniHasPasswordKey(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return false;
    const wchar_t sentinel[] = L"\x1F__absent__\x1F";
    wchar_t buffer[64]{};
    const DWORD length = GetPrivateProfileStringW(L"database", L"password", sentinel, buffer,
                                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
    return !(length == static_cast<DWORD>(std::wcslen(sentinel)) && std::wcsncmp(buffer, sentinel, length) == 0);
}

std::wstring ReadIniValue(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[512]{};
    const DWORD length = GetPrivateProfileStringW(section, key, L"", buffer,
                                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
    return std::wstring(buffer, length);
}

// Raw byte scan for both encodings the file can realistically have: our own writer emits UTF-16LE,
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

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

DataSourceDraft MySQLDraft(PasswordAction action, std::string newPassword = {}) {
    DataSourceDraft draft;
    draft.kind = DataSourceKind::MySql;
    draft.database.host = "db.example";
    draft.database.port = 3307;
    draft.database.username = "app";
    draft.database.database = "todolist";
    draft.passwordAction = action;
    draft.newPassword = std::move(newPassword);
    return draft;
}

}  // namespace

void RunSettingsServiceTests() {
    // --- 1. A fresh profile defaults to the local data source ---------------
    const auto freshRoot = MakeCaseRoot("fresh");
    {
        ConfigStore store(freshRoot / L"config.ini");
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        auto loaded = service.Load();
        Check("fresh profile loads", loaded.ok());
        Check("fresh profile defaults to Local",
              loaded.ok() && loaded.value().config.dataSource == DataSourceKind::Local);
        Check("fresh profile has no saved credential",
              loaded.ok() && loaded.value().credentialState == CredentialState::Missing);
        Check("fresh profile reports no legacy plaintext",
              loaded.ok() && !loaded.value().legacyPlaintextPresent);
        Check("load never writes the config file", !std::filesystem::exists(freshRoot / L"config.ini"));
    }
    Cleanup(freshRoot);

    // --- 2. Legacy INI (with plaintext password) keeps starting on MySQL ----
    const auto legacyRoot = MakeCaseRoot("legacy");
    const auto legacyPath = legacyRoot / L"config.ini";
    {
        WriteRawIni(legacyPath,
                    "[database]\nhost=legacy.example\nport=3307\nusername=olduser\npassword=old-plain\n"
                    "database=olddb\nconnect_timeout_seconds=7\n");
        ConfigStore store(legacyPath);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        auto loaded = service.Load();
        Check("legacy ini loads", loaded.ok());
        Check("legacy ini keeps MySQL as the data source",
              loaded.ok() && loaded.value().config.dataSource == DataSourceKind::MySql);
        Check("legacy ini flags the plaintext password",
              loaded.ok() && loaded.value().legacyPlaintextPresent);
        Check("legacy password is available at runtime",
              loaded.ok() && loaded.value().mysqlPassword == "old-plain");
        Check("legacy host is preserved", loaded.ok() && loaded.value().config.database.host == "legacy.example");
        Check("legacy password is not a stored credential",
              loaded.ok() && loaded.value().credentialState == CredentialState::Missing);
        Check("no vault write happens on load", vault.saveCount == 0);
    }
    Cleanup(legacyRoot);

    // --- 3. Explicit source=local beats the legacy database block -----------
    const auto explicitRoot = MakeCaseRoot("explicit");
    {
        const auto path = explicitRoot / L"config.ini";
        WriteRawIni(path, "[data]\nsource=local\n\n[database]\nhost=legacy.example\npassword=old-plain\n");
        ConfigStore store(path);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        auto loaded = service.Load();
        Check("explicit source=local wins over legacy keys",
              loaded.ok() && loaded.value().config.dataSource == DataSourceKind::Local);
        Check("explicit local still detects the pending plaintext",
              loaded.ok() && loaded.value().legacyPlaintextPresent);
    }
    Cleanup(explicitRoot);

    // --- 4. Migration happy path --------------------------------------------
    const auto migrateRoot = MakeCaseRoot("migrate");
    const auto migratePath = migrateRoot / L"config.ini";
    {
        WriteRawIni(migratePath, "[database]\nhost=localhost\nport=3306\nusername=root\npassword=migrate-me\n");
        ConfigStore store(migratePath);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("migration preload succeeds", service.Load().ok());
        const auto migrated = service.MigrateLegacyCredential();
        Check("migration succeeds", migrated.ok());
        Check("migration wrote exactly one credential", vault.saveCount >= 1);
        Check("migration stored the plaintext value", vault.value.has_value() && *vault.value == "migrate-me");
        Check("migration removed the password key from the ini", !IniHasPasswordKey(migratePath));
        Check("migration left no password bytes in the file", !RawFileMentionsPassword(migratePath));
        Check("migration keeps the mysql host", ReadBytes(migratePath).size() > 0);
        // A reload must now see a clean file and the credential as the source of truth.
        ConfigStore verifyStore(migratePath);
        FakeCredentialVault verifyVault;
        verifyVault.value = "migrate-me";
        SettingsService verifyService(verifyStore, verifyVault);
        auto reloaded = verifyService.Load();
        Check("post-migration reload has no legacy plaintext",
              reloaded.ok() && !reloaded.value().legacyPlaintextPresent);
        Check("post-migration reload reads the credential",
              reloaded.ok() && reloaded.value().credentialState == CredentialState::Present &&
                  reloaded.value().mysqlPassword == "migrate-me");
        Check("post-migration reload keeps MySQL",
              reloaded.ok() && reloaded.value().config.dataSource == DataSourceKind::MySql);
    }
    Cleanup(migrateRoot);

    // --- 5. Migration with an existing credential keeps it ------------------
    const auto migrateExistingRoot = MakeCaseRoot("migrate-existing");
    {
        const auto path = migrateExistingRoot / L"config.ini";
        WriteRawIni(path, "[database]\nhost=localhost\npassword=stale-plain\n");
        ConfigStore store(path);
        FakeCredentialVault vault;
        vault.value = "already-encrypted";
        SettingsService service(store, vault);
        Check("preload with existing credential succeeds", service.Load().ok());
        Check("existing credential wins over the plaintext",
              service.Load().ok() && service.RuntimePassword() == "already-encrypted");
        const auto migrated = service.MigrateLegacyCredential();
        Check("migration with an existing credential succeeds", migrated.ok());
        Check("migration does not overwrite the existing credential", *vault.value == "already-encrypted");
        Check("migration still drops the plaintext line", !IniHasPasswordKey(path));
    }
    Cleanup(migrateExistingRoot);

    // --- 6. Migration failure leaves the plaintext untouched ----------------
    const auto migrateFailRoot = MakeCaseRoot("migrate-fail");
    {
        const auto path = migrateFailRoot / L"config.ini";
        WriteRawIni(path, "[database]\nhost=localhost\npassword=keep-me-plain\n");
        const auto before = ReadBytes(path);
        ConfigStore store(path);
        FakeCredentialVault vault;
        vault.FailSave(ErrorCode::PermissionDenied);
        SettingsService service(store, vault);
        Check("failing migration preload succeeds", service.Load().ok());
        const auto migrated = service.MigrateLegacyCredential();
        Check("migration reports the vault failure", !migrated.ok() &&
                                                         migrated.error().code == ErrorCode::PermissionDenied);
        Check("failed migration keeps the ini byte identical", ReadBytes(path) == before);
        Check("failed migration keeps the password key", IniHasPasswordKey(path));
        Check("a failed migration still reports the retained plaintext", service.LegacyPasswordRetained());
        Check("failed migration stores nothing", !vault.value.has_value());
    }
    Cleanup(migrateFailRoot);

    // --- 7. Migration with nothing to migrate is a no-op --------------------
    const auto migrateNoopRoot = MakeCaseRoot("migrate-noop");
    {
        ConfigStore store(migrateNoopRoot / L"config.ini");
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("no-op migration preload succeeds", service.Load().ok());
        const auto migrated = service.MigrateLegacyCredential();
        Check("migration without plaintext succeeds", migrated.ok());
        Check("migration without plaintext writes no credential", vault.saveCount == 0);
    }
    Cleanup(migrateNoopRoot);

    // --- 8. Replace: credential first, then the INI ------------------------
    const auto replaceRoot = MakeCaseRoot("replace");
    const auto replacePath = replaceRoot / L"config.ini";
    {
        ConfigStore store(replacePath);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("replace preload succeeds", service.Load().ok());
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Replace, "brand-new"));
        Check("replace commit succeeds", committed.ok());
        Check("replace stored the new password", vault.value.has_value() && *vault.value == "brand-new");
        Check("replace keeps the password out of the ini", !IniHasPasswordKey(replacePath));
        Check("replace updates the runtime password", service.RuntimePassword() == "brand-new");
        Check("replace switches the in-memory data source",
              service.Config().dataSource == DataSourceKind::MySql);
        Check("replace persists source=mysql", ReadIniValue(replacePath, L"data", L"source") == L"mysql");
        ConfigStore verify(replacePath);
        auto reloaded = verify.Load();
        Check("replace is visible after reload",
              reloaded.ok() && reloaded.value().dataSource == DataSourceKind::MySql &&
                  reloaded.value().database.host == "db.example" && reloaded.value().database.port == 3307);
        Check("replace did not persist any password",
              reloaded.ok() && reloaded.value().database.password != "brand-new");
    }
    Cleanup(replaceRoot);

    // --- 9. Keep: never touches the vault ----------------------------------
    const auto keepRoot = MakeCaseRoot("keep");
    {
        ConfigStore store(keepRoot / L"config.ini");
        FakeCredentialVault vault;
        vault.value = "stored-secret";
        SettingsService service(store, vault);
        Check("keep preload succeeds", service.Load().ok());
        const int savesBefore = vault.saveCount;
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Keep));
        Check("keep commit succeeds", committed.ok());
        Check("keep does not write a credential", vault.saveCount == savesBefore);
        Check("keep uses the stored credential", service.RuntimePassword() == "stored-secret");
        Check("keep leaves the credential value alone", *vault.value == "stored-secret");
    }
    Cleanup(keepRoot);

    // --- 10. Keep with an unreadable credential must fail, not blank the password
    const auto keepBrokenRoot = MakeCaseRoot("keep-broken");
    {
        const auto path = keepBrokenRoot / L"config.ini";
        WriteRawIni(path, "[data]\nsource=mysql\n\n[database]\nhost=localhost\n");
        ConfigStore store(path);
        FakeCredentialVault vault;
        vault.value = "unreadable";
        vault.FailLoad(ErrorCode::CredentialUnavailable);
        SettingsService service(store, vault);
        auto loaded = service.Load();
        Check("unreadable credential still loads", loaded.ok());
        Check("unreadable credential is reported as unavailable",
              loaded.ok() && loaded.value().credentialState == CredentialState::Unavailable);
        Check("unreadable credential is flagged instead of becoming a password",
              loaded.ok() && loaded.value().mysqlPassword.empty() &&
                  loaded.value().credentialError.code == ErrorCode::CredentialUnavailable);
        const auto before = ReadBytes(path);
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Keep));
        Check("keep with an unreadable credential fails",
              !committed.ok() && committed.error().code == ErrorCode::CredentialUnavailable);
        Check("keep with an unreadable credential leaves the ini untouched", ReadBytes(path) == before);
    }
    Cleanup(keepBrokenRoot);

    // --- 11. Clear removes the credential ----------------------------------
    const auto clearRoot = MakeCaseRoot("clear");
    {
        ConfigStore store(clearRoot / L"config.ini");
        FakeCredentialVault vault;
        vault.value = "to-clear";
        SettingsService service(store, vault);
        Check("clear preload succeeds", service.Load().ok());
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Clear));
        Check("clear commit succeeds", committed.ok());
        Check("clear removed the credential", !vault.value.has_value());
        Check("clear reports a missing credential", service.Credential() == CredentialState::Missing);
        Check("clear leaves the runtime password empty", service.RuntimePassword().empty());
        Check("clear wrote no password to the ini", !IniHasPasswordKey(clearRoot / L"config.ini"));
    }
    Cleanup(clearRoot);

    // --- 12. Vault write failure blocks the INI write -----------------------
    const auto vaultFailRoot = MakeCaseRoot("vault-fail");
    {
        const auto path = vaultFailRoot / L"config.ini";
        ConfigStore store(path);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("vault-fail preload succeeds", service.Load().ok());
        Check("vault-fail baseline save succeeds",
              store.Save(service.Config()).ok());
        const auto before = ReadBytes(path);
        vault.FailSave(ErrorCode::PermissionDenied);
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Replace, "nope"));
        Check("vault write failure fails the commit",
              !committed.ok() && committed.error().code == ErrorCode::PermissionDenied);
        Check("vault write failure leaves the ini untouched", ReadBytes(path) == before);
        Check("vault write failure does not fake a saved credential", !vault.value.has_value());
        Check("vault write failure keeps the old data source",
              service.Config().dataSource == DataSourceKind::Local);
    }
    Cleanup(vaultFailRoot);

    // --- 13. INI write failure rolls the credential back ---------------------
    const auto iniFailRoot = MakeCaseRoot("ini-fail");
    {
        // A directory where the ini file should be makes every rewrite fail without touching ACLs.
        const auto blockedPath = iniFailRoot / L"blocked.ini";
        std::error_code error;
        std::filesystem::create_directories(blockedPath, error);
        ConfigStore store(blockedPath);
        FakeCredentialVault vault;
        vault.value = "previous-secret";
        SettingsService service(store, vault);
        (void)service.Load();
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Replace, "would-be-new"));
        Check("ini write failure fails the commit", !committed.ok());
        Check("ini write failure rolls the credential back",
              vault.value.has_value() && *vault.value == "previous-secret");
        Check("ini write failure does not adopt the new password",
              service.RuntimePassword() != "would-be-new");
    }
    Cleanup(iniFailRoot);

    // --- 14. Local switches never touch the credential ----------------------
    const auto localRoot = MakeCaseRoot("local");
    {
        const auto path = localRoot / L"config.ini";
        ConfigStore store(path);
        FakeCredentialVault vault;
        vault.value = "mysql-secret";
        SettingsService service(store, vault);
        Check("local preload succeeds", service.Load().ok());
        DataSourceDraft draft;
        draft.kind = DataSourceKind::Local;
        draft.passwordAction = PasswordAction::Clear;  // must be ignored for a local switch
        const int savesBefore = vault.saveCount;
        const int removesBefore = vault.removeCount;
        const auto committed = service.CommitConnection(draft);
        Check("local commit succeeds", committed.ok());
        Check("local switch ignores the password action", vault.saveCount == savesBefore &&
                                                              vault.removeCount == removesBefore);
        Check("local switch keeps the mysql credential", vault.value.has_value() &&
                                                            *vault.value == "mysql-secret");
        ConfigStore verify(path);
        auto reloaded = verify.Load();
        Check("local switch persists source=local",
              reloaded.ok() && reloaded.value().dataSource == DataSourceKind::Local);
        Check("local switch writes no password", !IniHasPasswordKey(path));
    }
    Cleanup(localRoot);

    // --- 15. Replace with an empty password is an explicit empty password ---
    const auto emptyRoot = MakeCaseRoot("empty");
    {
        ConfigStore store(emptyRoot / L"config.ini");
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("empty-password preload succeeds", service.Load().ok());
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Replace, ""));
        Check("explicit empty password commit succeeds", committed.ok());
        Check("explicit empty password is stored as a present credential",
              vault.value.has_value() && vault.value->empty());
        Check("explicit empty password keeps the state Present",
              service.Credential() == CredentialState::Present);
        Check("explicit empty password is not written to the ini", !IniHasPasswordKey(emptyRoot / L"config.ini"));
    }
    Cleanup(emptyRoot);

    // --- 16. A pending plaintext survives config rewrites until it migrates ---
    // (T14 changed the guard: ConfigStore used to refuse the rewrite with Conflict, which made every
    // window-position save fail and pop a toast. It now retains the line and succeeds.)
    const auto guardedRoot = MakeCaseRoot("guarded");
    {
        const auto path = guardedRoot / L"config.ini";
        WriteRawIni(path, "[database]\nhost=localhost\npassword=guarded-plain\n");
        ConfigStore store(path);
        auto loaded = store.Load();
        Check("guarded load succeeds", loaded.ok());
        Check("guarded store flags the plaintext", store.HasLegacyPlaintextPassword());
        Check("guarded plaintext can be taken", store.TakeLegacyPlaintextPassword().has_value() &&
                                                    *store.TakeLegacyPlaintextPassword() == "guarded-plain");
        const auto pending = store.Save(loaded.value());
        Check("save while the plaintext is pending succeeds", pending.ok());
        Check("pending save keeps the password key", IniHasPasswordKey(path));
        Check("pending save keeps the exact value",
              ReadIniValue(path, L"database", L"password") == L"guarded-plain");
        Check("pending save still reports the plaintext as retained", store.LegacyPasswordRetained());
        store.MarkLegacyPlaintextMigrated();
        Check("save succeeds once the migration is marked", store.Save(loaded.value()).ok());
        Check("marked save removes the password key", !IniHasPasswordKey(path));
        Check("marked save leaves no password bytes", !RawFileMentionsPassword(path));
    }
    Cleanup(guardedRoot);

    // --- 17. A commit migrates a pending plaintext before rewriting the INI ---
    const auto pendingCommitRoot = MakeCaseRoot("pending-commit");
    {
        const auto path = pendingCommitRoot / L"config.ini";
        WriteRawIni(path, "[database]\nhost=localhost\nport=3306\nusername=root\npassword=pending-plain\n");
        ConfigStore store(path);
        FakeCredentialVault vault;
        SettingsService service(store, vault);
        Check("pending-commit preload succeeds", service.Load().ok());
        const auto committed = service.CommitConnection(MySQLDraft(PasswordAction::Keep));
        Check("commit with a pending plaintext succeeds", committed.ok());
        Check("commit migrated the plaintext into the vault",
              vault.value.has_value() && *vault.value == "pending-plain");
        Check("commit dropped the plaintext line", !IniHasPasswordKey(path));
        Check("commit left no password bytes behind", !RawFileMentionsPassword(path));
        Check("commit clears the retained plaintext", !service.LegacyPasswordRetained());
        Check("commit keeps MySQL as the source", service.Config().dataSource == DataSourceKind::MySql);
        Check("commit keeps the runtime password", service.RuntimePassword() == "pending-plain");
    }
    Cleanup(pendingCommitRoot);
}
