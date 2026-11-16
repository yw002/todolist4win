#include <cstddef>
#include <string>
#include <vector>

#include "common/Sha256.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("Sha256", name, value, detail); }

std::vector<std::byte> ToBytes(const std::string& text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char value : text) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    return bytes;
}
}  // namespace

void RunSha256Tests() {
    // FIPS 180-4 published test vectors.
    Check("empty input",
          Sha256::Hex(std::string_view{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check("empty vector input",
          Sha256::Hex(std::vector<std::byte>{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check("abc",
          Sha256::Hex(std::string_view{"abc"}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Check("hello world",
          Sha256::Hex(std::string_view{"hello world"}) == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    Check("one million 'a'",
          Sha256::Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Both overloads must agree: the UI hashes a byte vector, the data layer may hash a string view.
    const std::string pngLike = "\x89PNG\r\n\x1a\nIHDR\x00\x01\x02\x03";
    const std::vector<std::byte> pngBytes = ToBytes(pngLike);
    Check("both overloads agree", Sha256::Hex(pngBytes) == Sha256::Hex(std::string_view{pngLike}));
    Check("result is 64 chars", Sha256::Hex(pngBytes).size() == 64);
    Check("result is lowercase", Sha256::Hex(pngBytes).find_first_of("ABCDEF") == std::string::npos);
    Check("hash is deterministic", Sha256::Hex(pngBytes) == Sha256::Hex(ToBytes(pngLike)));
    Check("different bytes give a different key",
          Sha256::Hex(ToBytes(pngLike)) != Sha256::Hex(ToBytes(pngLike + "x")));

    // --- Canonical key rule --------------------------------------------------
    Check("canonical key accepted", Sha256::IsCanonicalKey(Sha256::Hex(pngBytes)));
    Check("all-zero key accepted", Sha256::IsCanonicalKey(std::string(64, '0')));
    Check("uppercase rejected", !Sha256::IsCanonicalKey(std::string(64, 'A')));
    Check("mixed case rejected", !Sha256::IsCanonicalKey(std::string(63, 'a') + "F"));
    Check("short key rejected", !Sha256::IsCanonicalKey(std::string(63, 'a')));
    Check("long key rejected", !Sha256::IsCanonicalKey(std::string(65, 'a')));
    Check("non-hex rejected", !Sha256::IsCanonicalKey(std::string(63, 'a') + "g"));
    Check("empty key rejected", !Sha256::IsCanonicalKey(""));
}
