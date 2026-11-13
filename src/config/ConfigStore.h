#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "common/Result.h"
#include "config/AppConfig.h"

namespace todo {

// Reads and writes config.ini. Since T13 this store never *writes* the MySQL password: the durable
// copy lives in the DPAPI credential file (see DpapiCredentialVault), and a plaintext
// [database] password= line can only exist in a file written by an older version of the app.
//
// The legacy plaintext is still read, because it is the migration source: Load() reports the value
// in DatabaseConfig::password and remembers that the file still contains it. While that is the
// case, Save() keeps the original line in the rewritten file instead of dropping it - it is not
// writing a secret, it is refusing to destroy the only existing copy. Save() still succeeds, so a
// window-position save can never fail just because the migration has not happened yet (that would
// pop a toast on every drag). SettingsService clears the flag with MarkLegacyPlaintextMigrated()
// once DpapiCredentialVault::Save has succeeded; the next Save() then no longer re-emits the line,
// which is how the plaintext finally disappears from disk.
class ConfigStore {
public:
    ConfigStore();
    explicit ConfigStore(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] Result<AppConfig> Load() const;
    // Whole-file rewrite through a sibling .tmp + MoveFileExW(MOVEFILE_REPLACE_EXISTING). Never
    // writes a password= line; re-emits the existing one verbatim while the legacy plaintext is
    // still pending, so the secret survives until the vault holds an encrypted copy.
    [[nodiscard]] Result<void> Save(const AppConfig& config) const;
    [[nodiscard]] static std::filesystem::path DefaultPath();

    // True when the file still contains a plaintext [database] password= line, as observed by the
    // last Load() (or by a later TakeLegacyPlaintextPassword()). Never exposes the value itself.
    [[nodiscard]] bool HasLegacyPlaintextPassword() const noexcept;

    // The same question, answered from the file on disk instead of the cached flag. This is what
    // the startup path asks ("do I have to warn the user?"), because a flag left over from an
    // earlier Load() must never decide whether a secret still exists. Const: it reads the ini and
    // refreshes the cached flag, it never writes the configuration.
    [[nodiscard]] bool LegacyPasswordRetained() const noexcept;

    // The plaintext password still stored in the INI, ready to be handed to the credential vault.
    // Returns nullopt when there is none; that also refreshes the flag from disk. The value is not
    // logged and is never written back anywhere by this class.
    [[nodiscard]] std::optional<std::string> TakeLegacyPlaintextPassword() const;

    // Called by SettingsService after the credential vault holds a valid encrypted copy: the next
    // Save() stops re-emitting the plaintext line, which is what removes it from disk.
    void MarkLegacyPlaintextMigrated() noexcept;

private:
    std::filesystem::path path_;
    mutable bool legacyPlaintextPresent_{false};
};

}  // namespace todo
