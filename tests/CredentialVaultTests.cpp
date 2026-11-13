#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "config/AppPaths.h"
#include "config/DpapiCredentialVault.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") {
    ReportTest("CredentialVault", name, value, detail);
}

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_cred_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" +
                             std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

// Every credential used by the tests lives under AppPaths::ForRoots(temp, temp): nothing touches the
// real %APPDATA% / %LOCALAPPDATA% of whoever runs the build.
DpapiCredentialVault MakeVault(const std::filesystem::path& root) {
    return DpapiCredentialVault(AppPaths::ForRoots(root, root));
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::vector<unsigned char> bytes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return bytes;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return bytes;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t ReadUint32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    if (bytes.size() < offset + 4) return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void WriteUint32(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xFFu);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<unsigned char>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<unsigned char>((value >> 24) & 0xFFu);
}

bool ContainsAscii(const std::vector<unsigned char>& bytes, const std::string& needle) {
    if (needle.empty() || bytes.size() < needle.size()) return false;
    const std::vector<unsigned char> pattern(needle.begin(), needle.end());
    return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

// Hex dump of the container header, reported alongside the check so the layout is visible in the
// test log instead of only being asserted.
std::string HeaderHex(const std::vector<unsigned char>& bytes) {
    static const char* kDigits = "0123456789ABCDEF";
    const std::size_t limit = bytes.size() < 24 ? bytes.size() : 24;
    std::string text;
    for (std::size_t index = 0; index < limit; ++index) {
        if (index != 0) text.push_back(' ');
        text.push_back(kDigits[bytes[index] >> 4]);
        text.push_back(kDigits[bytes[index] & 0x0F]);
    }
    return text;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

}  // namespace

void RunCredentialVaultTests() {
    // --- Missing file is a normal state, not an error -----------------------
    const auto missingRoot = MakeCaseRoot("missing");
    {
        auto vault = MakeVault(missingRoot);
        auto loaded = vault.Load();
        Check("missing file loads successfully", loaded.ok());
        Check("missing file yields no password", loaded.ok() && !loaded.value().has_value());
        auto exists = vault.Exists();
        Check("missing file reports not saved", exists.ok() && !exists.value());
        Check("load does not create the credential directory",
              !std::filesystem::exists(missingRoot / L"credentials"));
        Check("remove of a missing file succeeds", vault.Remove().ok());
    }
    Cleanup(missingRoot);

    // --- Container header layout (design 3.3) -------------------------------
    const auto layoutRoot = MakeCaseRoot("layout");
    {
        auto vault = MakeVault(layoutRoot);
        Check("save creates the credential file", vault.Save("layout-secret").ok());
        const auto bytes = ReadBytes(vault.Path());
        const std::string header = HeaderHex(bytes);
        Check("header magic is TDLWCRED",
              bytes.size() >= 8 && std::memcmp(bytes.data(), "TDLWCRED", 8) == 0, header.c_str());
        Check("header version is 1", ReadUint32(bytes, 8) == 1);
        Check("header algorithm is 1 (DPAPI user scope)", ReadUint32(bytes, 12) == 1);
        Check("header reserved field is zero", ReadUint32(bytes, 20) == 0);
        const std::uint32_t payload = ReadUint32(bytes, 16);
        Check("payload length matches the file size", bytes.size() == 24ull + payload);
        Check("payload length is inside the sanity limit", payload > 0 && payload <= 64u * 1024u);
        Check("ciphertext does not contain the plaintext", !ContainsAscii(bytes, "layout-secret"));
        Check("no temporary file is left behind",
              !std::filesystem::exists(vault.Path().parent_path() / L"mysql.cred.tmp"));
        auto exists = vault.Exists();
        Check("exists reports the saved credential", exists.ok() && exists.value());
    }
    Cleanup(layoutRoot);

    // --- Round trips --------------------------------------------------------
    const auto roundTripRoot = MakeCaseRoot("roundtrip");
    {
        auto vault = MakeVault(roundTripRoot);
        Check("save succeeds", vault.Save("s3cr3t-pa55word").ok());
        auto loaded = vault.Load();
        Check("load succeeds", loaded.ok());
        Check("round trip returns the same password",
              loaded.ok() && loaded.value().has_value() && *loaded.value() == "s3cr3t-pa55word");

        // Saving again must fully replace the previous blob.
        Check("second save succeeds", vault.Save("another-password").ok());
        auto reloaded = vault.Load();
        Check("second save wins",
              reloaded.ok() && reloaded.value().has_value() && *reloaded.value() == "another-password");

        // Empty password: a stored empty string is Present, not Missing.
        Check("saving an empty password succeeds", vault.Save("").ok());
        auto empty = vault.Load();
        Check("empty password loads as a present credential", empty.ok() && empty.value().has_value());
        Check("empty password stays empty", empty.ok() && empty.value().has_value() && empty.value()->empty());

        // Long password: DPAPI has no small limit of its own, we exercise a 8 KiB secret.
        const std::string longPassword(8192, 'L');
        Check("saving a long password succeeds", vault.Save(longPassword).ok());
        auto longLoaded = vault.Load();
        Check("long password round trips", longLoaded.ok() && longLoaded.value().has_value() &&
                                               *longLoaded.value() == longPassword);

        // Non-ASCII must survive byte-for-byte (the vault is byte oriented, not text oriented).
        const std::string unicodePassword = "密码-ñ-✓";
        Check("saving a non-ASCII password succeeds", vault.Save(unicodePassword).ok());
        auto unicodeLoaded = vault.Load();
        Check("non-ASCII password round trips",
              unicodeLoaded.ok() && unicodeLoaded.value().has_value() && *unicodeLoaded.value() == unicodePassword);
    }
    Cleanup(roundTripRoot);

    // --- Corruption matrix: every case must fail, none may crash ------------
    // Each case starts from a healthy file so that exactly one property is broken.
    const auto corruptRoot = MakeCaseRoot("corrupt");
    {
        auto vault = MakeVault(corruptRoot);
        const auto good = [&]() {
            (void)vault.Save("corrupt-base");
            return ReadBytes(vault.Path());
        };

        // Tampered ciphertext byte.
        auto tampered = good();
        if (tampered.size() > 40) tampered[24 + (tampered.size() - 24) / 2] ^= 0xFF;
        WriteBytes(vault.Path(), tampered);
        auto tamperedLoad = vault.Load();
        Check("tampered ciphertext is rejected", !tamperedLoad.ok());
        Check("tampered ciphertext reports CredentialUnavailable",
              !tamperedLoad.ok() && tamperedLoad.error().code == ErrorCode::CredentialUnavailable);

        // Header only, no payload at all.
        auto headerOnly = good();
        headerOnly.resize(24);
        WriteUint32(headerOnly, 16, 0);
        WriteBytes(vault.Path(), headerOnly);
        auto headerOnlyLoad = vault.Load();
        Check("zero payload length is rejected",
              !headerOnlyLoad.ok() && headerOnlyLoad.error().code == ErrorCode::CredentialUnavailable);

        // Truncated file (shorter than the header).
        auto truncated = good();
        truncated.resize(20);
        WriteBytes(vault.Path(), truncated);
        auto truncatedLoad = vault.Load();
        Check("file shorter than the header is rejected",
              !truncatedLoad.ok() && truncatedLoad.error().code == ErrorCode::CredentialUnavailable);

        // Declared payload longer than the file.
        auto shortPayload = good();
        shortPayload.resize(shortPayload.size() - 1);
        WriteBytes(vault.Path(), shortPayload);
        auto shortLoad = vault.Load();
        Check("truncated payload is rejected",
              !shortLoad.ok() && shortLoad.error().code == ErrorCode::CredentialUnavailable);

        // Declared payload longer than the sanity limit.
        auto oversized = good();
        WriteUint32(oversized, 16, 0x7FFFFFF0u);
        WriteBytes(vault.Path(), oversized);
        auto oversizedLoad = vault.Load();
        Check("oversized payload length is rejected",
              !oversizedLoad.ok() && oversizedLoad.error().code == ErrorCode::CredentialUnavailable);

        // Wrong magic.
        auto badMagic = good();
        std::memcpy(badMagic.data(), "NOTACRED", 8);
        WriteBytes(vault.Path(), badMagic);
        auto badMagicLoad = vault.Load();
        Check("unknown magic is rejected",
              !badMagicLoad.ok() && badMagicLoad.error().code == ErrorCode::CredentialUnavailable);

        // Future container version.
        auto badVersion = good();
        WriteUint32(badVersion, 8, 2);
        WriteBytes(vault.Path(), badVersion);
        auto badVersionLoad = vault.Load();
        Check("unknown version is rejected",
              !badVersionLoad.ok() && badVersionLoad.error().code == ErrorCode::CredentialUnavailable);

        // Unknown algorithm: no guessing, no silent downgrade.
        auto badAlgorithm = good();
        WriteUint32(badAlgorithm, 12, 7);
        WriteBytes(vault.Path(), badAlgorithm);
        auto badAlgorithmLoad = vault.Load();
        Check("unknown algorithm is rejected",
              !badAlgorithmLoad.ok() && badAlgorithmLoad.error().code == ErrorCode::CredentialUnavailable);

        // Empty file on disk.
        WriteBytes(vault.Path(), {});
        auto emptyFileLoad = vault.Load();
        Check("empty file is rejected",
              !emptyFileLoad.ok() && emptyFileLoad.error().code == ErrorCode::CredentialUnavailable);

        // Random bytes that happen to be long enough.
        std::vector<unsigned char> garbage(64, 0xAB);
        std::memcpy(garbage.data(), "TDLWCRED", 8);
        WriteUint32(garbage, 8, 1);
        WriteUint32(garbage, 12, 1);
        WriteUint32(garbage, 16, 40);
        WriteBytes(vault.Path(), garbage);
        auto garbageLoad = vault.Load();
        Check("random payload is rejected",
              !garbageLoad.ok() && garbageLoad.error().code == ErrorCode::CredentialUnavailable);

        // A rejected file must never be reported as an empty password.
        Check("rejected credential is not reported as an empty password", !garbageLoad.ok());

        // The corrupt file must be left alone so the user can still diagnose it.
        Check("corrupt file is not deleted by a failed load", std::filesystem::exists(vault.Path()));

        // A successful save is allowed to overwrite the corrupt file (explicit user action).
        Check("save overwrites a corrupt file", vault.Save("recovered").ok());
        auto recoveredLoad = vault.Load();
        Check("overwritten credential round trips",
              recoveredLoad.ok() && recoveredLoad.value().has_value() && *recoveredLoad.value() == "recovered");
    }
    Cleanup(corruptRoot);

    // --- Remove semantics ---------------------------------------------------
    const auto removeRoot = MakeCaseRoot("remove");
    {
        auto vault = MakeVault(removeRoot);
        Check("save before remove succeeds", vault.Save("to-be-removed").ok());
        Check("remove succeeds", vault.Remove().ok());
        auto exists = vault.Exists();
        Check("credential is gone after remove", exists.ok() && !exists.value());
        auto loaded = vault.Load();
        Check("load after remove yields no password", loaded.ok() && !loaded.value().has_value());
        Check("second remove is idempotent", vault.Remove().ok());
    }
    Cleanup(removeRoot);

    // --- Directory is created on demand -------------------------------------
    const auto nestedParent = MakeCaseRoot("nested");
    const auto nestedRoot = nestedParent / L"deeper" / L"still";
    {
        auto vault = MakeVault(nestedRoot);
        Check("save creates missing directories", vault.Save("nested-secret").ok());
        auto loaded = vault.Load();
        Check("nested credential round trips",
              loaded.ok() && loaded.value().has_value() && *loaded.value() == "nested-secret");
    }
    Cleanup(nestedParent);
}
