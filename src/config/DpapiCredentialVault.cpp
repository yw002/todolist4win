#include "config/DpapiCredentialVault.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace todo {
namespace {

// Container magic. Kept as raw bytes (not a wchar_t string) so the file layout is byte exact and
// independent of the compiler's wchar_t size.
constexpr unsigned char kMagic[8] = {'T', 'D', 'L', 'W', 'C', 'R', 'E', 'D'};

// Domain separation for CryptProtectData. This is *not* a secret: it only prevents this blob from
// being interchangeable with other DPAPI blobs of the same Windows account. The protection itself
// comes from the user's DPAPI master key.
constexpr char kEntropy[] = "TodoListWin|MySQLCredential|v1";

// Fixed length of the entropy string; the terminating NUL is not part of the entropy.
constexpr DWORD kEntropyLength = static_cast<DWORD>(sizeof(kEntropy) - 1);

std::uint32_t ReadUint32(const std::vector<unsigned char>& bytes, std::size_t offset) {
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

// Maps a Win32 file error onto the project error codes. Access problems must stay distinguishable
// from "the file is not there" and from "the content is unusable".
ErrorCode MapFileError(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
        case ERROR_WRITE_FAULT:
            return ErrorCode::PermissionDenied;
        default:
            return ErrorCode::Internal;
    }
}

// Reads a whole file into memory. On failure 'error' receives the raw Win32 code so callers can
// still distinguish a missing file from an unreadable one.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<unsigned char>& bytes, DWORD& error) {
    bytes.clear();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        error = GetLastError();
        CloseHandle(handle);
        return false;
    }
    // The container caps the payload at 64 KiB, so a "file" that is far larger is corrupt by
    // definition; refusing to allocate avoids a surprise multi-gigabyte read from a bad path.
    constexpr LONGLONG kSaneLimit = 1024LL * 1024LL;
    if (size.QuadPart > kSaneLimit) {
        error = ERROR_FILE_INVALID;
        CloseHandle(handle);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        if (!ReadFile(handle, bytes.data() + offset, remaining, &chunk, nullptr)) {
            error = GetLastError();
            CloseHandle(handle);
            bytes.clear();
            return false;
        }
        if (chunk == 0) break;
        offset += chunk;
    }
    CloseHandle(handle);
    bytes.resize(offset);
    error = ERROR_SUCCESS;
    return true;
}

// Writes through a sibling temporary file and replaces the target with MoveFileExW. A reader either
// sees the previous complete file or the new complete file, never a half-written credential.
Result<void> WriteWholeFileAtomically(const std::filesystem::path& target,
                                      const std::vector<unsigned char>& bytes) {
    const auto temporary = target.parent_path() / (target.filename().wstring() + L".tmp");
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return Result<void>::Failure({MapFileError(code), "无法创建凭据临时文件，请检查凭据目录权限",
                                      static_cast<int>(code)});
    }
    bool writeFailed = false;
    DWORD code = ERROR_SUCCESS;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        if (!WriteFile(handle, bytes.data() + offset, remaining, &chunk, nullptr) || chunk == 0) {
            writeFailed = true;
            code = GetLastError();
            break;
        }
        offset += chunk;
    }
    if (!writeFailed && !FlushFileBuffers(handle)) {
        writeFailed = true;
        code = GetLastError();
    }
    CloseHandle(handle);
    if (writeFailed) {
        DeleteFileW(temporary.c_str());
        return Result<void>::Failure({MapFileError(code), "无法写入凭据文件", static_cast<int>(code)});
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        code = GetLastError();
        DeleteFileW(temporary.c_str());
        return Result<void>::Failure({MapFileError(code), "无法原子替换凭据文件", static_cast<int>(code)});
    }
    return Result<void>::Success();
}

}  // namespace

DpapiCredentialVault::DpapiCredentialVault(AppPaths paths)
    : paths_(std::move(paths)), filePath_(paths_.CredentialsPath()) {}

Result<std::optional<std::string>> DpapiCredentialVault::Load() {
    std::vector<unsigned char> raw;
    DWORD error = ERROR_SUCCESS;
    if (!ReadWholeFile(filePath_, raw, error)) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            // No file is a normal, non-error state: the user simply has not saved a password yet.
            return Result<std::optional<std::string>>::Success(std::nullopt);
        }
        return Result<std::optional<std::string>>::Failure(
            {MapFileError(error), "无法读取已保存的 MySQL 凭据文件", static_cast<int>(error)});
    }

    // --- container header ---------------------------------------------------
    if (raw.size() < kHeaderSize) {
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据文件不完整（长度小于文件头）"});
    }
    if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) != 0) {
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据文件标识不正确"});
    }
    const std::uint32_t version = ReadUint32(raw, 8);
    if (version != kVersion) {
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据格式版本不受支持"});
    }
    const std::uint32_t algorithm = ReadUint32(raw, 12);
    if (algorithm != kAlgorithmDpapiUserScope) {
        // No guessing and no downgrade: an unknown algorithm means we cannot claim to know what the
        // bytes mean, so the caller must ask the user for the password again.
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据加密算法不受支持"});
    }
    const std::uint32_t payloadLength = ReadUint32(raw, 16);
    if (payloadLength == 0 || payloadLength > kMaxPayloadBytes) {
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据长度不合法"});
    }
    if (raw.size() != kHeaderSize + static_cast<std::size_t>(payloadLength)) {
        // Truncated or padded: the declared length is authoritative, so a mismatch is corruption.
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable, "已保存的 MySQL 凭据文件长度与文件头不一致"});
    }

    // --- DPAPI --------------------------------------------------------------
    DATA_BLOB cipher{};
    cipher.pbData = raw.data() + kHeaderSize;
    cipher.cbData = payloadLength;
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    entropy.cbData = kEntropyLength;
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&cipher, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        const DWORD code = GetLastError();
        // Integrity check failures, a different Windows account or a different machine all land
        // here. All of them mean "a secret exists that we cannot read", never "empty password".
        return Result<std::optional<std::string>>::Failure(
            {ErrorCode::CredentialUnavailable,
             "已保存的 MySQL 凭据无法解密，可能由其他 Windows 账户或另一台计算机创建", static_cast<int>(code)});
    }
    std::string password;
    if (plain.pbData != nullptr && plain.cbData > 0) {
        password.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
        SecureZeroMemory(plain.pbData, plain.cbData);
    }
    if (plain.pbData != nullptr) LocalFree(plain.pbData);
    // An empty string here is a legitimate stored password, and Load() reports it as Present.
    return Result<std::optional<std::string>>::Success(std::optional<std::string>(std::move(password)));
}

Result<void> DpapiCredentialVault::Save(const std::string& password) {
    const auto directoryReady = paths_.EnsureCredentialsDirectory();
    if (!directoryReady.ok()) return directoryReady;

    // CryptProtectData takes a mutable pointer but does not modify the input buffer.
    DATA_BLOB plain{};
    plain.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(password.data()));
    plain.cbData = static_cast<DWORD>(password.size());
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    entropy.cbData = kEntropyLength;
    DATA_BLOB cipher{};
    // No CRYPTPROTECT_LOCAL_MACHINE: the blob must be bound to this user, so that copying
    // %LOCALAPPDATA% to another account or machine yields an undecryptable file instead of leaking
    // the password. CRYPTPROTECT_UI_FORBIDDEN keeps the switch silent (no system credential dialog).
    if (!CryptProtectData(&plain, L"TodoListWin MySQL credential", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &cipher)) {
        const DWORD code = GetLastError();
        return Result<void>::Failure({ErrorCode::Internal, "无法加密 MySQL 密码", static_cast<int>(code)});
    }
    if (cipher.pbData == nullptr || cipher.cbData == 0 || cipher.cbData > kMaxPayloadBytes) {
        if (cipher.pbData != nullptr) LocalFree(cipher.pbData);
        return Result<void>::Failure({ErrorCode::Internal, "加密后的凭据长度不合法"});
    }

    std::vector<unsigned char> bytes(kHeaderSize + cipher.cbData, 0);
    std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
    WriteUint32(bytes, 8, kVersion);
    WriteUint32(bytes, 12, kAlgorithmDpapiUserScope);
    WriteUint32(bytes, 16, cipher.cbData);
    WriteUint32(bytes, 20, 0);  // reserved, must stay 0
    std::memcpy(bytes.data() + kHeaderSize, cipher.pbData, cipher.cbData);
    SecureZeroMemory(cipher.pbData, cipher.cbData);
    LocalFree(cipher.pbData);

    return WriteWholeFileAtomically(filePath_, bytes);
}

Result<void> DpapiCredentialVault::Remove() {
    if (DeleteFileW(filePath_.c_str())) return Result<void>::Success();
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        // Removing something that is not there is the requested end state.
        return Result<void>::Success();
    }
    return Result<void>::Failure({MapFileError(code), "无法删除已保存的 MySQL 凭据", static_cast<int>(code)});
}

Result<bool> DpapiCredentialVault::Exists() const {
    const DWORD attributes = GetFileAttributesW(filePath_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return Result<bool>::Success(false);
        }
        return Result<bool>::Failure({MapFileError(code), "无法检查凭据文件状态", static_cast<int>(code)});
    }
    return Result<bool>::Success((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

}  // namespace todo
