#pragma once

#include <optional>
#include <string>

#include "common/Result.h"

namespace todo {

// What the application knows about the stored MySQL credential. It is deliberately a separate
// concept from "the password string": "there is a secret we cannot read" (Unavailable) must never
// collapse into "there is no secret" (Missing) or into an empty password.
enum class CredentialState {
    Missing,      // no credential file (or the user cleared it)
    Present,      // a credential file exists and decrypts; the password may legitimately be empty
    Unavailable   // a credential file exists but cannot be read/decrypted by this user/machine
};

// Storage for the MySQL password.
//
// This interface is platform neutral on purpose: it lives in src/config and is included by
// SettingsService and by tests, so it must not pull in <windows.h> or any DPAPI type. The only
// implementation that knows about Windows is DpapiCredentialVault.
//
// Contract for every implementation:
//   * Load() succeeds with std::nullopt when no credential is stored. That is not an error and not
//     an empty password.
//   * Load() succeeds with a value (possibly the empty string) when a credential is stored and was
//     read back successfully. An empty string is a real, usable password.
//   * Load() fails with ErrorCode::CredentialUnavailable when a stored credential cannot be
//     recovered. Parse and decryption failures must never be reported as "no credential".
//   * Errors never contain the password, the ciphertext or any part of it.
class ICredentialVault {
public:
    virtual ~ICredentialVault() = default;

    [[nodiscard]] virtual Result<std::optional<std::string>> Load() = 0;
    [[nodiscard]] virtual Result<void> Save(const std::string& password) = 0;
    [[nodiscard]] virtual Result<void> Remove() = 0;
    [[nodiscard]] virtual Result<bool> Exists() const = 0;
};

}  // namespace todo
