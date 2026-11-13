#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "config/AppPaths.h"
#include "config/CredentialVault.h"

namespace todo {

// DPAPI-backed credential vault.
//
// The password is encrypted with CryptProtectData in the *current user's* scope (no
// CRYPTPROTECT_LOCAL_MACHINE, no CRYPTPROTECT_SYSTEM) and CRYPTPROTECT_UI_FORBIDDEN, so nothing
// can pop a system dialog while the app switches data sources. A fixed application entropy string
// is mixed in, which only separates this blob from other DPAPI users of the same account; it is
// not a secret and cannot replace the DPAPI protection.
//
// On-disk container (all integers little-endian, see docs/DATA-SOURCE-DESIGN.md 3.3):
//   0   8   magic "TDLWCRED"
//   8   4   version         = 1
//   12  4   algorithm       = 1 (DPAPI, current-user scope)
//   16  4   payload_length  = byte length of the DPAPI blob
//   20  4   reserved        = 0
//   24  N   DPAPI blob
//
// The header is explicit so a future algorithm change is detected instead of mis-parsed. Anything
// that does not match the header exactly is reported as CredentialUnavailable; the file is never
// silently rewritten or deleted, so the user can still diagnose and re-enter the password.
class DpapiCredentialVault final : public ICredentialVault {
public:
    static constexpr std::size_t kHeaderSize = 24;
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kAlgorithmDpapiUserScope = 1;
    // A password is a short secret; anything beyond this is a corrupt length field, not a password.
    static constexpr std::uint32_t kMaxPayloadBytes = 64u * 1024u;

    // Paths come from AppPaths so tests can point at a temporary directory:
    //   DpapiCredentialVault vault(AppPaths::ForRoots(temp, temp));
    explicit DpapiCredentialVault(AppPaths paths);

    [[nodiscard]] Result<std::optional<std::string>> Load() override;
    [[nodiscard]] Result<void> Save(const std::string& password) override;
    [[nodiscard]] Result<void> Remove() override;
    [[nodiscard]] Result<bool> Exists() const override;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return filePath_; }

private:
    AppPaths paths_;
    std::filesystem::path filePath_;
};

}  // namespace todo
