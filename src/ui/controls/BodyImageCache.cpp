#include "ui/controls/BodyImageCache.h"

#include <algorithm>
#include <utility>

#include "ui/ClipboardImage.h"

namespace todo::ui {

void BodyImageCache::SetImages(const std::vector<todo::BodyImage>& images) {
    cpu_.clear();
    cpu_.reserve(images.size());
    for (const auto& image : images) {
        if (image.key.empty() || image.bytes.empty()) continue;
        CpuImage stored;
        stored.bytes = image.bytes;
        stored.width = image.width;
        stored.height = image.height;
        cpu_.insert_or_assign(image.key, std::move(stored));
    }
    // Drop derived views that no longer have a CPU-side source. Keys that survive are kept: their
    // PNG bytes are unchanged (key == SHA-256 of bytes), so the bitmap is still valid.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (cpu_.find(it->first) == cpu_.end()) {
            totalDecodedBytes_ -= it->second.decodedBytes;
            lru_.erase(it->second.lru);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void BodyImageCache::AddImage(const todo::BodyImage& image) {
    if (image.key.empty() || image.bytes.empty()) return;
    CpuImage stored;
    stored.bytes = image.bytes;
    stored.width = image.width;
    stored.height = image.height;
    cpu_.insert_or_assign(image.key, std::move(stored));
}

void BodyImageCache::Clear() noexcept {
    cpu_.clear();
    entries_.clear();
    lru_.clear();
    totalDecodedBytes_ = 0;
}

bool BodyImageCache::Has(const std::string& key) const noexcept {
    return cpu_.find(key) != cpu_.end();
}

void BodyImageCache::Touch(const std::string& key) {
    auto found = entries_.find(key);
    if (found == entries_.end()) return;
    lru_.splice(lru_.begin(), lru_, found->second.lru);
    found->second.lru = lru_.begin();
}

void BodyImageCache::EvictIfNeeded() noexcept {
    while (!lru_.empty() && (entries_.size() > kMaxEntries || totalDecodedBytes_ > kMaxDecodedBytes)) {
        const std::string victim = lru_.back();
        lru_.pop_back();
        auto found = entries_.find(victim);
        if (found == entries_.end()) continue;
        totalDecodedBytes_ -= std::min(totalDecodedBytes_, found->second.decodedBytes);
        entries_.erase(found);
    }
}

bool BodyImageCache::Paint(ID2D1DeviceContext& context, RenderResources& resources,
                           const std::string& key, const D2D1_RECT_F& rect) {
    // A device loss/rebuild replaces every D2D resource. Derived bitmaps must not survive it; the
    // CPU-side PNG bytes stay valid and are re-decoded lazily on the next Paint.
    const std::uint64_t generation = resources.DeviceGeneration();
    if (!generationInitialised_ || observedGeneration_ != generation) {
        entries_.clear();
        lru_.clear();
        totalDecodedBytes_ = 0;
        observedGeneration_ = generation;
        generationInitialised_ = true;
    }
    const auto source = cpu_.find(key);
    if (source == cpu_.end() || source->second.bytes.empty()) return false;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return false;

    auto cached = entries_.find(key);
    if (cached == entries_.end()) {
        // CreateBitmapFromPng is the single premultiplication point (PNG straight BGRA -> 32bppPBGRA)
        // and accepts void* + size, so std::byte* needs no conversion or copy.
        const auto created = CreateBitmapFromPng(context, source->second.bytes.data(), source->second.bytes.size());
        if (!created.ok()) return false;
        const std::uint64_t decoded = static_cast<std::uint64_t>(std::max(1u, source->second.width)) *
                                      std::max(1u, source->second.height) * 4u;
        lru_.push_front(key);
        Entry entry;
        entry.bitmap = created.value;
        entry.decodedBytes = decoded;
        entry.lru = lru_.begin();
        entries_.emplace(key, std::move(entry));
        totalDecodedBytes_ += decoded;
        EvictIfNeeded();
        cached = entries_.find(key);
        if (cached == entries_.end()) return false;  // immediately evicted (oversized image)
    } else {
        Touch(key);
    }
    context.DrawBitmap(cached->second.bitmap.Get(), &rect, 1.0F, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    return true;
}

}  // namespace todo::ui
