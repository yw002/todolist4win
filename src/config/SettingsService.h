#pragma once

#include <string>

#include "common/Result.h"
#include "config/AppConfig.h"
#include "config/ConfigStore.h"
#include "config/CredentialVault.h"

namespace todo {

// Everything the application needs to know about the persisted configuration after a successful
// load. The password is carried in memory only; it is never written back to config.ini.
struct LoadedSettings {
    AppConfig config;
    std::string mysqlPassword;                              // effective runtime password
    CredentialState credentialState{CredentialState::Missing};
    bool legacyPlaintextPresent{false};                     // INI still has password= (migration pending)
    AppError credentialError{};                             // filled when CredentialState::Unavailable

    [[nodiscard]] bool hasSavedCredential() const noexcept {
        return credentialState == CredentialState::Present;
    }
};

// Combines ConfigStore (plain INI) with ICredentialVault (encrypted password) and owns the two
// rules that must not be duplicated anywhere else:
//
//   1. PasswordAction {Keep, Replace, Clear} is resolved here, in exactly one function. No caller,
//      no vault and no dialog may reintroduce an implicit "empty string means keep" convention.
//   2. Write order is credential first, INI second. The INI is the only thing that can be rewritten
//      atomically together with the "migration done" flag, so if the INI write fails the freshly
//      written credential is rolled back on a best-effort basis and the caller sees an error
//      instead of a half-migrated state.
//
// The class knows nothing about UI, Win32 or DPAPI: it talks to the abstract vault, which is what
// makes the whole matrix (keep/replace/clear x credential present/absent/failing) testable offline.
class SettingsService {
public:
    SettingsService(ConfigStore configStore, ICredentialVault& vault);

    // Loads the INI and the credential. Succeeds even when the credential cannot be decrypted: the
    // result carries CredentialState::Unavailable plus the error, because a broken credential must
    // not stop the local data source from starting. The caller decides what to do about MySQL.
    [[nodiscard]] Result<LoadedSettings> Load();

    // Moves a legacy plaintext [database] password= into the vault and rewrites the INI without it.
    // When the INI already carries no password this is a no-op. On a vault failure the INI is left
    // untouched, so the plaintext is preserved for a later retry and nothing pretends to be
    // encrypted.
    [[nodiscard]] Result<void> MigrateLegacyCredential();

    // Applies a data source draft: resolves the effective password, writes the credential when the
    // action requires it, then writes the INI. Local switches never touch the credential, so a
    // user can go back to MySQL later and still find the saved password.
    [[nodiscard]] Result<void> CommitConnection(const DataSourceDraft& draft);

    // Resolves the password a switch would use, without writing anything. The hot switch needs the
    // effective password to build and probe the candidate worker *before* anything is committed
    // (design 6.2 step 3); a failure here means no candidate may be created at all.
    //
    // A local switch never reads the vault on purpose: a MySQL credential that cannot be decrypted
    // must not prevent switching to a perfectly usable local database.
    [[nodiscard]] Result<std::string> PreviewPassword(const DataSourceDraft& draft);

    [[nodiscard]] const AppConfig& Config() const noexcept { return config_; }
    [[nodiscard]] CredentialState Credential() const noexcept { return credentialState_; }
    // True when config.ini still holds a plaintext password line, asked from the file itself rather
    // than from a flag left over by an earlier load. The startup path uses it to decide whether the
    // user has to be told once that the migration did not happen (design 2.4).
    [[nodiscard]] bool LegacyPasswordRetained() const;
    // The password currently valid for MySQL in this process; empty is a legal value, and
    // CredentialState tells "no saved password" apart from "stored empty password".
    [[nodiscard]] const std::string& RuntimePassword() const noexcept { return runtimePassword_; }

private:
    // Keep -> stored credential, else the password already in memory.
    // Replace -> the draft value, including the empty string.
    // Clear -> the empty string, for an explicit "no password" MySQL account.
    [[nodiscard]] Result<std::string> ResolvePassword(const DataSourceDraft& draft);

    ConfigStore configStore_;
    ICredentialVault& vault_;
    AppConfig config_{};
    std::string runtimePassword_;
    CredentialState credentialState_{CredentialState::Missing};
};

}  // namespace todo
