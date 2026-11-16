#include "domain/BodyDocument.h"

#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

#include "common/Sha256.h"
#include "domain/Json.h"
#include "domain/TaskRules.h"

namespace todo {
namespace {

[[nodiscard]] AppError Failure(ErrorCode code, const char* message) { return AppError{code, message, 0}; }

// Numbers in a v1 document are small non-negative integers. Anything with a fraction, exponent or
// sign is rejected here rather than silently truncated by strtoull.
[[nodiscard]] bool ParseUnsigned(const json::Value* value, std::uint64_t& out) {
    if (value == nullptr || value->kind != json::Kind::Number || value->numberText.empty()) return false;
    for (const char character : value->numberText) {
        if (character < '0' || character > '9') return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value->numberText.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = static_cast<std::uint64_t>(parsed);
    return true;
}

// Unknown *members* of a known block are echoed back verbatim so a newer writer's marks/style/alt
// extensions survive a v1 save. memberRaws holds the exact `"key":value` source slices.
[[nodiscard]] std::string PreserveUnknownMembers(const json::Value& object, std::initializer_list<std::string_view> known) {
    std::string out;
    for (std::size_t i = 0; i < object.memberKeys.size() && i < object.memberRaws.size(); ++i) {
        bool isKnown = false;
        for (const std::string_view key : known) {
            if (object.memberKeys[i] == key) { isKnown = true; break; }
        }
        if (isKnown) continue;
        if (!out.empty()) out.push_back(',');
        out += object.memberRaws[i];
    }
    return out;
}

[[nodiscard]] bool IsValidPreservedFragment(const std::string& fragment, bool wholeObject) {
    const std::string candidate = wholeObject ? fragment : "{" + fragment + "}";
    if (candidate.empty()) return true;
    const auto parsed = json::Parse(candidate);
    return parsed.ok() && parsed.value().IsObject();
}

[[nodiscard]] std::string WithPreservedMembers(const std::string& canonicalMembers, const std::string& rawJson) {
    std::string out = "{";
    out += canonicalMembers;
    if (!rawJson.empty()) {
        out.push_back(',');
        out += rawJson;
    }
    out.push_back('}');
    return out;
}

}  // namespace

BodyDocument BodyDocument::Empty() {
    BodyDocument document;
    BodyBlock block;
    block.kind = BodyBlockKind::Text;
    document.blocks.push_back(std::move(block));
    return document;
}

BodyDocument BodyDocument::FromLegacyNote(const std::string& note) {
    BodyDocument document = Empty();
    document.legacy = true;
    document.blocks.front().text = note;
    return document;
}

Result<BodyDocument> BodyDocument::Parse(const std::string& json) {
    if (json.empty()) return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "正文文档为空"));
    if (json.size() > kMaxBodyDocBytes) {
        return Result<BodyDocument>::Failure(Failure(ErrorCode::BodyTooLarge, "正文超过 256 KB 上限"));
    }
    const auto root = json::Parse(json);
    if (!root.ok()) return Result<BodyDocument>::Failure(root.error());
    if (!root.value().IsObject()) {
        return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "正文根对象格式无效"));
    }
    std::uint64_t version = 0;
    if (!ParseUnsigned(root.value().Find("v"), version)) {
        return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "正文缺少有效的格式版本号"));
    }
    if (version != static_cast<std::uint64_t>(kBodyDocVersion)) {
        // Never guess at a newer format and never discard its blocks. Callers degrade to read-only.
        return Result<BodyDocument>::Failure(Failure(ErrorCode::BodySchemaUnsupported,
                                                    "正文格式不受当前版本支持，请升级应用"));
    }
    const json::Value* blocks = root.value().Find("blocks");
    if (blocks == nullptr || !blocks->IsArray()) {
        return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "正文缺少 blocks 数组"));
    }
    if (blocks->elements.size() > kMaxBlocks) {
        return Result<BodyDocument>::Failure(Failure(ErrorCode::BodyTooLarge, "正文块数量超过上限"));
    }

    BodyDocument document;
    document.version = static_cast<int>(version);
    document.legacy = false;
    for (const json::Value& blockValue : blocks->elements) {
        if (!blockValue.IsObject()) {
            return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "正文块必须是对象"));
        }
        const std::string* type = blockValue.FindString("t");
        if (type == nullptr) {
            // A block without a recognisable type is kept verbatim. It is never parsed or rewritten.
            BodyBlock unknown;
            unknown.kind = BodyBlockKind::Unknown;
            unknown.rawJson = blockValue.raw;
            document.blocks.push_back(std::move(unknown));
            continue;
        }
        if (*type == "text") {
            const std::string* text = blockValue.FindString("text");
            if (text == nullptr) {
                return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "文本块缺少 text 字段"));
            }
            BodyBlock block;
            block.kind = BodyBlockKind::Text;
            block.text = *text;
            block.rawJson = PreserveUnknownMembers(blockValue, {"t", "text"});
            document.blocks.push_back(std::move(block));
            continue;
        }
        if (*type == "image") {
            const std::string* key = blockValue.FindString("key");
            const std::string* mime = blockValue.FindString("mime");
            std::uint64_t width = 0;
            std::uint64_t height = 0;
            if (key == nullptr || mime == nullptr || !ParseUnsigned(blockValue.Find("w"), width) ||
                !ParseUnsigned(blockValue.Find("h"), height) || width > 0xFFFFFFFFULL || height > 0xFFFFFFFFULL) {
                return Result<BodyDocument>::Failure(Failure(ErrorCode::InvalidInput, "图片块字段不完整或格式无效"));
            }
            BodyBlock block;
            block.kind = BodyBlockKind::Image;
            block.image.key = *key;
            // mime is stored verbatim and never validated against "image/png": a future JPEG/WebP
            // block must survive a v1 read/save round trip without being rejected here.
            block.image.mime = *mime;
            block.image.width = static_cast<std::uint32_t>(width);
            block.image.height = static_cast<std::uint32_t>(height);
            if (const std::string* alt = blockValue.FindString("alt")) block.image.alt = *alt;
            block.rawJson = PreserveUnknownMembers(blockValue, {"t", "key", "mime", "w", "h", "alt"});
            document.blocks.push_back(std::move(block));
            continue;
        }
        BodyBlock unknown;
        unknown.kind = BodyBlockKind::Unknown;
        unknown.rawJson = blockValue.raw;
        document.blocks.push_back(std::move(unknown));
    }
    return Result<BodyDocument>::Success(std::move(document));
}

std::string BodyDocument::ToJson() const {
    std::string out = "{\"v\":";
    out += std::to_string(version);
    out += ",\"blocks\":[";
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i != 0) out.push_back(',');
        const BodyBlock& block = blocks[i];
        if (block.kind == BodyBlockKind::Unknown) {
            if (block.rawJson.empty()) out += "{}";
            else out += block.rawJson;
        } else if (block.kind == BodyBlockKind::Text) {
            std::string members = "\"t\":\"text\",\"text\":";
            members += json::Quote(block.text);
            out += WithPreservedMembers(members, block.rawJson);
        } else {
            std::string members = "\"t\":\"image\",\"key\":";
            members += json::Quote(block.image.key);
            members += ",\"mime\":";
            members += json::Quote(block.image.mime);
            members += ",\"w\":";
            members += std::to_string(block.image.width);
            members += ",\"h\":";
            members += std::to_string(block.image.height);
            if (!block.image.alt.empty()) {
                members += ",\"alt\":";
                members += json::Quote(block.image.alt);
            }
            out += WithPreservedMembers(members, block.rawJson);
        }
    }
    out += "]}";
    return out;
}

std::string BodyDocument::PlainText(bool withImagePlaceholders) const {
    std::string out;
    // Blocks are concatenated in document order with no separator: the text blocks themselves carry
    // their own newlines, and note is only an index/projection, never the source of truth.
    for (const BodyBlock& block : blocks) {
        if (block.kind == BodyBlockKind::Text) out += block.text;
        else if (block.kind == BodyBlockKind::Image && withImagePlaceholders) {
            out.append(kImagePlaceholder.data(), kImagePlaceholder.size());
        }
    }
    return out;
}

std::vector<std::string> BodyDocument::ImageKeys() const {
    std::vector<std::string> keys;
    std::set<std::string> seen;
    for (const BodyBlock& block : blocks) {
        if (block.kind != BodyBlockKind::Image || block.image.key.empty()) continue;
        if (seen.insert(block.image.key).second) keys.push_back(block.image.key);
    }
    return keys;
}

Result<void> BodyDocument::Validate() const {
    if (version != kBodyDocVersion) {
        return Result<void>::Failure(Failure(ErrorCode::BodySchemaUnsupported, "正文格式不受当前版本支持，请升级应用"));
    }
    if (blocks.size() > kMaxBlocks) {
        return Result<void>::Failure(Failure(ErrorCode::BodyTooLarge, "正文块数量超过上限"));
    }
    if (ToJson().size() > kMaxBodyDocBytes) {
        return Result<void>::Failure(Failure(ErrorCode::BodyTooLarge, "正文超过 256 KB 上限"));
    }
    std::size_t imageCount = 0;
    for (const BodyBlock& block : blocks) {
        if (block.kind == BodyBlockKind::Text) {
            if (!json::IsValidUtf8(block.text)) {
                return Result<void>::Failure(Failure(ErrorCode::InvalidInput, "正文含非法 UTF-8 编码"));
            }
        } else if (block.kind == BodyBlockKind::Image) {
            ++imageCount;
            if (!Sha256::IsCanonicalKey(block.image.key)) {
                return Result<void>::Failure(Failure(ErrorCode::ImageInvalid, "图片引用 key 必须是 64 位小写十六进制 SHA-256"));
            }
            if (block.image.width == 0 || block.image.width > kMaxImageDimension ||
                block.image.height == 0 || block.image.height > kMaxImageDimension) {
                return Result<void>::Failure(Failure(ErrorCode::ImageTooLarge, "图片边长超过 8192 像素"));
            }
            if (static_cast<std::uint64_t>(block.image.width) * block.image.height > kMaxImagePixels) {
                return Result<void>::Failure(Failure(ErrorCode::ImageTooLarge, "图片像素总量超过 32 M"));
            }
            if (block.image.mime.empty()) {
                return Result<void>::Failure(Failure(ErrorCode::ImageInvalid, "图片块缺少 mime 字段"));
            }
            if (!json::IsValidUtf8(block.image.alt)) {
                return Result<void>::Failure(Failure(ErrorCode::InvalidInput, "图片说明含非法 UTF-8 编码"));
            }
        }
        if (!IsValidPreservedFragment(block.rawJson, block.kind == BodyBlockKind::Unknown)) {
            return Result<void>::Failure(Failure(ErrorCode::InvalidInput, "正文含无法安全回写的未知字段"));
        }
    }
    if (imageCount > kMaxImagesPerTask) {
        return Result<void>::Failure(Failure(ErrorCode::BodyTooLarge, "单个任务最多 20 张图片"));
    }
    // The projection is what ValidateTask()/ValidateNote() will check on the read path, so reject it
    // here with the same rule instead of letting a later read fail.
    const auto note = TaskRules::ValidateNote(PlainText(false));
    if (!note.ok()) return Result<void>::Failure(note.error());
    return Result<void>::Success();
}

}  // namespace todo
