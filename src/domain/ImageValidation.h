#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Result.h"
#include "common/Sha256.h"
#include "domain/BodyDocument.h"

namespace todo {

// The same guard runs in both image repositories and in TaskService before any write. It is the last
// line of defence behind the clipboard layer: never truncate, never downsample, never repair a bad
// key silently. A key that does not match the PNG bytes would make the document unreadable later.
inline Result<void> ValidateBodyImage(const BodyImage& image) {
    if (!Sha256::IsCanonicalKey(image.key)) {
        return Result<void>::Failure({ErrorCode::ImageInvalid, "图片引用 key 必须是 64 位小写十六进制 SHA-256"});
    }
    if (image.bytes.empty() || image.bytes.size() > kMaxImageBytes) {
        return Result<void>::Failure({ErrorCode::ImageTooLarge, "图片超过 4 MB，请压缩后再粘贴"});
    }
    if (Sha256::Hex(image.bytes) != image.key) {
        return Result<void>::Failure({ErrorCode::ImageInvalid, "图片 key 与 PNG 内容不匹配"});
    }
    if (image.width == 0 || image.width > kMaxImageDimension || image.height == 0 || image.height > kMaxImageDimension) {
        return Result<void>::Failure({ErrorCode::ImageTooLarge, "图片边长超过 8192 像素"});
    }
    if (static_cast<std::uint64_t>(image.width) * image.height > kMaxImagePixels) {
        return Result<void>::Failure({ErrorCode::ImageTooLarge, "图片像素总量超过 32 M"});
    }
    if (image.mime.empty()) {
        return Result<void>::Failure({ErrorCode::ImageInvalid, "图片缺少 mime 类型"});
    }
    return Result<void>::Success();
}

inline Result<void> ValidateNewImages(const std::vector<BodyImage>& images) {
    if (images.size() > kMaxImagesPerTask) {
        return Result<void>::Failure({ErrorCode::BodyTooLarge, "单个任务最多 20 张图片"});
    }
    std::uint64_t total = 0;
    for (const BodyImage& image : images) {
        const auto checked = ValidateBodyImage(image);
        if (!checked.ok()) return checked;
        total += image.bytes.size();
    }
    if (total > kMaxTaskImageBytes) {
        return Result<void>::Failure({ErrorCode::BodyTooLarge, "单个任务图片总量超过 16 MB"});
    }
    return Result<void>::Success();
}

}  // namespace todo
