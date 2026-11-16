#pragma once

// LRU cache of PNG bytes -> ID2D1Bitmap for body images.
//
// CPU-side PNG bytes (std::vector<todo::BodyImage>) are the only source of truth. The bitmaps in
// this cache are derived views: on a device generation change the whole cache is dropped and the
// bitmaps are rebuilt from the still-available CPU bytes, so no database read is needed after a
// device loss. Bitmaps are created exclusively through ClipboardImage::CreateBitmapFromPng(), which
// performs the single premultiplication required by DXGI_ALPHA_MODE_PREMULTIPLIED.

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1_1.h>
#include <wrl/client.h>

#include "domain/BodyDocument.h"
#include "ui/RenderResources.h"

namespace todo::ui {

class BodyImageCache final {
public:
    static constexpr std::size_t kMaxEntries = 8;
    static constexpr std::uint64_t kMaxDecodedBytes = 32ull * 1024ull * 1024ull;

    BodyImageCache() = default;
    ~BodyImageCache() = default;
    BodyImageCache(const BodyImageCache&) = delete;
    BodyImageCache& operator=(const BodyImageCache&) = delete;

    // Replaces the CPU-side image set. Old cached bitmaps whose keys are gone are dropped; a
    // database reload must call Clear() first so no stale bitmap survives.
    void SetImages(const std::vector<todo::BodyImage>& images);
    // Adds one image without touching the rest. A freshly pasted image only exists in the editor
    // until Save, so it never appears in SetImages() (which mirrors the database) and would render
    // as the "unavailable" placeholder without this.
    void AddImage(const todo::BodyImage& image);
    void Clear() noexcept;

    // Draws `key` into `rect` (aspect-preserving fit is the caller's job: pass the final rect).
    // Returns false when the blob is missing, too large or could not be decoded; the caller then
    // renders the gray "图片不可用" placeholder. Never throws across the UI frame.
    bool Paint(ID2D1DeviceContext& context, RenderResources& resources,
               const std::string& key, const D2D1_RECT_F& rect);

    [[nodiscard]] bool Has(const std::string& key) const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept { return entries_.size(); }

private:
    struct CpuImage {
        std::vector<std::byte> bytes;
        std::uint32_t width{0};
        std::uint32_t height{0};
    };
    struct Entry {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t decodedBytes{0};
        std::list<std::string>::iterator lru;
    };

    void EvictIfNeeded() noexcept;
    void Touch(const std::string& key);

    std::unordered_map<std::string, CpuImage> cpu_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;  // front = most recently used
    std::uint64_t totalDecodedBytes_{0};
    std::uint64_t observedGeneration_{0};
    bool generationInitialised_{false};
};

}  // namespace todo::ui
