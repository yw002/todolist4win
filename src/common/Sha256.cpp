#include "common/Sha256.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>

namespace todo {
namespace {

std::string ToLowerHex(const BYTE* digest, DWORD length) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 2U);
    for (DWORD i = 0; i < length; ++i) {
        out.push_back(kDigits[(digest[i] >> 4) & 0x0FU]);
        out.push_back(kDigits[digest[i] & 0x0FU]);
    }
    return out;
}

// Returns an empty string on any WinCrypt failure. The caller treats an empty key as ImageInvalid
// rather than storing a broken reference; no secret or image bytes ever reach the error text.
std::string Hash(const void* data, std::size_t size) {
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) return {};
    HCRYPTHASH hash = 0;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        return {};
    }
    // A zero-length buffer is legal for SHA-256; CryptHashData rejects a null pointer, so pass a
    // dummy byte with a zero count instead of dereferencing anything the caller gave us.
    static constexpr BYTE kEmptyPlaceholder = 0;
    const BYTE* pointer = size == 0 ? &kEmptyPlaceholder : static_cast<const BYTE*>(data);
    const BOOL hashed = CryptHashData(hash, pointer, static_cast<DWORD>(size), 0);
    BYTE digest[32]{};
    DWORD digestLength = sizeof(digest);
    const BOOL read = hashed && CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLength, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!read || digestLength != sizeof(digest)) return {};
    return ToLowerHex(digest, digestLength);
}

}  // namespace

std::string Sha256::Hex(const std::vector<std::byte>& bytes) {
    return Hash(bytes.data(), bytes.size());
}

std::string Sha256::Hex(std::string_view bytes) {
    return Hash(bytes.data(), bytes.size());
}

}  // namespace todo
