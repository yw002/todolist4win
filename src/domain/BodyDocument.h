#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/Result.h"

namespace todo {

// ---------------------------------------------------------------------------------------------
// Single source of truth for every body/image limit. The UI reads these constants; it must never
// copy the numbers. All byte counts are UTF-8 bytes, not characters.
// ---------------------------------------------------------------------------------------------
inline constexpr int kBodyDocVersion = 1;
inline constexpr std::size_t kMaxBodyDocBytes = 256 * 1024;
inline constexpr std::size_t kMaxBlocks = 128;
inline constexpr std::size_t kImageKeyHexBytes = 64;
inline constexpr std::size_t kMaxImageBytes = 4 * 1024 * 1024;      // 4 MiB per PNG
inline constexpr std::uint32_t kMaxImageDimension = 8192;
inline constexpr std::uint64_t kMaxImagePixels = 32ull * 1024 * 1024;  // decompression-bomb gate
inline constexpr std::size_t kMaxImagesPerTask = 20;
inline constexpr std::size_t kMaxTaskImageBytes = 16 * 1024 * 1024;
inline constexpr std::string_view kImagePlaceholder = "〔图片〕";

enum class BodyBlockKind { Text, Image, Unknown };

struct BodyImageRef {
    std::string key;                 // 64 lowercase hex chars = SHA-256 of the PNG bytes
    std::string mime{"image/png"};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::string alt;
};

// `rawJson` plays two roles so that a v1 save never erases data written by a newer version:
//   * Unknown block  -> the verbatim source slice of the whole block object, e.g. {"t":"table",...}.
//   * Text/Image block -> the verbatim source slices of unknown *members* only, joined with commas
//     and without the enclosing braces, e.g. "marks":[...],"style":"b". ToJson appends them after
//     the canonical members. Both forms are read-only and are emitted unchanged.
struct BodyBlock {
    BodyBlockKind kind{BodyBlockKind::Text};
    std::string text;                // Text
    BodyImageRef image;              // Image
    std::string rawJson;             // see above; empty when there is nothing to preserve
};

// A PNG that crosses the DbWorker thread boundary. Contains bytes only - no COM, D2D or clipboard
// handles. `key` is the 64-char lowercase hex SHA-256 of `bytes`.
struct BodyImage {
    std::string key;
    std::string mime{"image/png"};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::byte> bytes;
};

// Payload of one body edit: only newly pasted images carry bytes; existing blobs are referenced by
// key. `doc` is the compact v1 JSON produced by BodyDocument::ToJson().
struct BodyEdit {
    std::string doc;
    std::vector<BodyImage> newImages;
};

struct BodyDocument {
    int version{kBodyDocVersion};
    bool legacy{false};              // true = synthesised from note; body_doc was SQL NULL
    std::vector<BodyBlock> blocks;

    // Pure functions. Parse never throws; it reports malformed JSON, invalid UTF-8, wrong root type,
    // missing blocks array, or v>1 as Result failures (v>1 -> BodySchemaUnsupported).
    [[nodiscard]] static Result<BodyDocument> Parse(const std::string& json);
    [[nodiscard]] static BodyDocument FromLegacyNote(const std::string& note);
    [[nodiscard]] static BodyDocument Empty();
    [[nodiscard]] std::string ToJson() const;
    [[nodiscard]] std::string PlainText(bool withImagePlaceholders) const;
    [[nodiscard]] std::vector<std::string> ImageKeys() const;   // de-duplicated, insertion order
    [[nodiscard]] Result<void> Validate() const;                // all limits + key format
};

}  // namespace todo
