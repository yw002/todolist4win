#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace todo {

// SHA-256 over raw bytes, returned as 64 lowercase hex characters. Used on both sides of the
// DbWorker boundary so the UI and the data layer derive the same `image_key` for identical PNGs.
// Implemented with WinCrypt (crypt32.lib), which is already linked by both projects.
class Sha256 {
public:
    [[nodiscard]] static std::string Hex(const std::vector<std::byte>& bytes);
    [[nodiscard]] static std::string Hex(std::string_view bytes);

    // True only for exactly 64 characters, all in [0-9a-f]. Uppercase is deliberately rejected:
    // image_key is the lowercase canonical form. Inline so the pure domain layer can use the same
    // rule without linking crypt32.
    [[nodiscard]] static bool IsCanonicalKey(std::string_view key) noexcept {
        if (key.size() != 64) return false;
        for (const char value : key) {
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
        }
        return true;
    }
};

}  // namespace todo
