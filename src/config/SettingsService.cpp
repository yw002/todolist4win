#include "config/SettingsService.h"

#include <optional>
#include <string>
#include <utility>

namespace todo {

SettingsService::SettingsService(ConfigStore configStore, ICredentialVault& vault)
    : configStore_(std::move(configStore)), vault_(vault) {}

Result<LoadedSettings> SettingsService::Load() {
    auto loaded = configStore_.Load();
    if (!loaded.ok()) return Result<LoadedSettings>::Failure(loaded.error());

    LoadedSettings settings;
    settings.config = loaded.value();
    settings.legacyPlaintextPresent = configStore_.HasLegacyPlaintextPassword();
    settings.credentialState = CredentialState::Missing;
    // The password already in memory: the legacy plaintext value, or the PRD default for a fresh
    // install. Used only until a stored credential is found.
    settings.mysqlPassword = settings.config.database.password;

    auto stored = vault_.Load();
    if (!stored.ok()) {
        // A credential file exists but cannot be recovered. Keep it visible as a state instead of
        // failing the whole load: the local data source must still start.
        settings.credentialState = CredentialState::Unavailable;
        settings.credentialError = stored.error();
        settings.mysqlPassword.clear();
    } else if (stored.value().has_value()) {
        settings.credentialState = CredentialState::Present;
        settings.mysqlPassword = *stored.value();
    }

    config_ = settings.config;
    config_.database.password = settings.mysqlPassword;
    runtimePassword_ = settings.mysqlPassword;
    credentialState_ = settings.credentialState;
    return Result<LoadedSettings>::Success(settings);
}

Result<void> SettingsService::MigrateLegacyCredential() {
    auto loaded = configStore_.Load();
    if (!loaded.ok()) return Result<void>::Failure(loaded.error());
    config_ = loaded.value();
    runtimePassword_ = config_.database.password;

    if (!configStore_.HasLegacyPlaintextPassword()) {
        // Nothing to migrate. Refresh the credential state so callers can rely on it afterwards.
        auto stored = vault_.Load();
        if (stored.ok()) {
            credentialState_ = stored.value().has_value() ? CredentialState::Present : CredentialState::Missing;
            if (stored.value().has_value()) {
                runtimePassword_ = *stored.value();
                config_.database.password = runtimePassword_;
            }
        } else {
            credentialState_ = CredentialState::Unavailable;
        }
        return Result<void>::Success();
    }

    const std::optional<std::string> plaintext = configStore_.TakeLegacyPlaintextPassword();
    if (!plaintext.has_value()) {
        // The flag was set by an older Load() but the line is gone from disk already.
        configStore_.MarkLegacyPlaintextMigrated();
        return Result<void>::Success();
    }

    auto exists = vault_.Exists();
    if (!exists.ok()) return Result<void>::Failure(exists.error());
    if (!exists.value()) {
        // Credential first: only a successfully encrypted copy may be followed by an INI rewrite.
        auto saved = vault_.Save(*plaintext);
        if (!saved.ok()) {
            // Leave the INI exactly as it is. The plaintext stays available for the next attempt and
            // the caller can show a real error instead of a fake "already encrypted" state.
            return Result<void>::Failure(saved.error());
        }
    }

    runtimePassword_ = *plaintext;
    config_.database.password = *plaintext;
    credentialState_ = CredentialState::Present;
    // The encrypted copy is in place, so the plaintext may now leave the INI. Marking has to happen
    // before Save(): ConfigStore re-emits the legacy line for as long as it believes the migration
    // is pending, so the flag is exactly what makes the rewrite drop it.
    configStore_.MarkLegacyPlaintextMigrated();
    auto written = configStore_.Save(config_);
    if (!written.ok()) {
        // The credential is valid, so nothing is lost; the INI still carries the plaintext and the
        // next start simply retries. Report the failure rather than claiming success.
        return Result<void>::Failure(written.error());
    }
    return Result<void>::Success();
}

Result<std::string> SettingsService::ResolvePassword(const DataSourceDraft& draft) {
    switch (draft.passwordAction) {
        case PasswordAction::Clear:
            // Explicit "this account has no password": an empty password is a usable value, and the
            // credential file is removed by CommitConnection so the state becomes Missing.
            return Result<std::string>::Success(std::string{});
        case PasswordAction::Replace:
            // Authoritative, including the empty string. No fallback, no implicit "keep".
            return Result<std::string>::Success(draft.newPassword);
        case PasswordAction::Keep:
            break;
    }
    // Keep: the encrypted copy is the source of truth when it exists.
    auto stored = vault_.Load();
    if (!stored.ok()) {
        // Cannot tell what the password is; never substitute an empty string for a secret we cannot
        // read, because that would look like a successful connection attempt with a blank password.
        return Result<std::string>::Failure(stored.error());
    }
    if (stored.value().has_value()) return Result<std::string>::Success(*stored.value());
    // No saved credential yet: continue with the value already loaded from the INI (legacy
    // plaintext) or the PRD default. This is not an implicit "empty means keep" rule - the value is
    // simply the one this process has always been using.
    return Result<std::string>::Success(runtimePassword_);
}

bool SettingsService::LegacyPasswordRetained() const { return configStore_.LegacyPasswordRetained(); }

Result<std::string> SettingsService::PreviewPassword(const DataSourceDraft& draft) {
    // Local: the candidate is a SQLite factory, so the MySQL password is irrelevant. Reading the
    // vault here would let a credential that cannot be decrypted block a switch to a local database
    // that works perfectly well.
    if (draft.kind != DataSourceKind::MySql) return Result<std::string>::Success(runtimePassword_);
    return ResolvePassword(draft);
}

Result<void> SettingsService::CommitConnection(const DataSourceDraft& draft) {
    // A file that still carries a plaintext password would keep carrying it: ConfigStore re-emits
    // that line verbatim on every rewrite until the migration is marked. The encrypted copy has to
    // exist before the INI is allowed to forget the secret, so finish the migration first. The flag
    // is re-read from disk here: a value left over from an earlier Load() must not decide whether a
    // secret gets dropped.
    if (configStore_.TakeLegacyPlaintextPassword().has_value()) {
        const auto migrated = MigrateLegacyCredential();
        if (!migrated.ok()) return migrated;
    }

    AppConfig next = config_;
    next.dataSource = draft.kind;
    next.database = draft.database;

    const bool mySql = draft.kind == DataSourceKind::MySql;
    std::string effectivePassword = runtimePassword_;
    if (mySql) {
        auto resolved = ResolvePassword(draft);
        if (!resolved.ok()) return Result<void>::Failure(resolved.error());
        effectivePassword = resolved.value();
    }
    // Local switches keep the MySQL password that is already in memory: the design says the saved
    // credential is preserved so a later switch back to MySQL still works.
    next.database.password = effectivePassword;

    // --- 1. credential ------------------------------------------------------
    // Only MySQL carries a credential, and Keep means "do not touch the stored file at all".
    const bool touchesCredential = mySql && draft.passwordAction != PasswordAction::Keep;
    std::optional<std::string> previousCredential;
    bool snapshotAvailable = false;
    if (touchesCredential) {
        // Snapshot for a best-effort rollback in case the INI write below fails.
        auto snapshot = vault_.Load();
        if (snapshot.ok()) {
            snapshotAvailable = true;
            previousCredential = snapshot.value();
        }
        if (draft.passwordAction == PasswordAction::Clear) {
            auto removed = vault_.Remove();
            if (!removed.ok()) return Result<void>::Failure(removed.error());
            credentialState_ = CredentialState::Missing;
        } else {
            auto saved = vault_.Save(effectivePassword);
            if (!saved.ok()) {
                // Nothing was written to the INI yet, so the previous state is still fully intact.
                return Result<void>::Failure(saved.error());
            }
            credentialState_ = CredentialState::Present;
        }
    }

    // --- 2. INI -------------------------------------------------------------
    auto written = configStore_.Save(next);
    if (!written.ok()) {
        if (touchesCredential && snapshotAvailable) {
            // Best effort only: the credential is not transactional, but leaving a brand-new secret
            // behind while the config still points at the old one would be worse than restoring.
            if (previousCredential.has_value()) {
                (void)vault_.Save(*previousCredential);
                credentialState_ = CredentialState::Present;
            } else {
                (void)vault_.Remove();
                credentialState_ = CredentialState::Missing;
            }
        }
        return Result<void>::Failure(written.error());
    }

    config_ = next;
    runtimePassword_ = effectivePassword;
    return Result<void>::Success();
}

}  // namespace todo
