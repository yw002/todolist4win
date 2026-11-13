#include "ui/CursorFactory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace todo::CursorFactory {
namespace {

// Classic pointer outline, in 0..1 shape space (origin top-left, y down). Keeping the familiar
// silhouette matters: the user must still recognise the shape as a mouse pointer.
constexpr float kArrowShape[] = {
    0.05F, 0.02F,  // tip (also the hotspot)
    0.05F, 0.70F,  // bottom of the long left edge
    0.21F, 0.54F,  // inner notch
    0.31F, 0.80F,  // tail, bottom-left
    0.44F, 0.74F,  // tail, bottom-right
    0.33F, 0.50F,  // second notch
    0.52F, 0.45F,  // right point
};
constexpr int kArrowPointCount = static_cast<int>(sizeof(kArrowShape) / (2 * sizeof(float)));

constexpr int kSubSamples = 4;  // 4x4 supersampling per pixel gives smooth edges
// Soft drop shadow outside the outline. A hard two-pixel halo (the previous design) read as a
// sticker; three fading rings read as a lifted glass edge instead.
constexpr int kShadowRadius = 3;

constexpr D2D1_COLOR_F kWhite{1.0F, 1.0F, 1.0F, 1.0F};
// A hair of warmth keeps the contour from looking like a printed black line on the cream theme.
constexpr D2D1_COLOR_F kContour{0.11F, 0.085F, 0.07F, 1.0F};

// Cache: one cursor per (DPI, accent colour) pair. Bounded so a pathological theme animation
// cannot grow the cache without limit; entries are recycled around the ring.
struct CacheEntry {
    UINT dpi{0};
    DWORD accentKey{0};
    HCURSOR cursor{nullptr};
};
constexpr std::size_t kCacheSize = 4;
CacheEntry g_cache[kCacheSize];
std::size_t g_nextSlot = 0;

void StorePixel(std::uint8_t* pixel, const D2D1_COLOR_F& color, float alpha) noexcept {
    // Cursor bitmaps are consumed as premultiplied BGRA (AlphaBlend semantics).
    const float clamped = std::clamp(alpha, 0.0F, 1.0F);
    const auto channel = [clamped](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * clamped * 255.0F));
    };
    pixel[0] = channel(color.b);
    pixel[1] = channel(color.g);
    pixel[2] = channel(color.r);
    pixel[3] = static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

bool InsidePolygon(const float* points, int count, float x, float y) noexcept {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const float xi = points[i * 2], yi = points[i * 2 + 1];
        const float xj = points[j * 2], yj = points[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

DWORD AccentKey(const D2D1_COLOR_F& accent) noexcept {
    const auto channel = [](float value) { return static_cast<DWORD>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)) & 0xFFU; };
    return (channel(accent.r) << 16) | (channel(accent.g) << 8) | channel(accent.b);
}

HCURSOR BuildArrow(int size, const D2D1_COLOR_F& accent) noexcept {
    // The arrow is deliberately colour-neutral: an accent-tinted tip was tried and read as a dirty
    // smudge at 32px. Accent only keeps the cache key honest for future per-theme variants.
    (void)accent;
    const auto pixelCount = static_cast<std::size_t>(size) * size;
    std::vector<float> coverage(pixelCount, 0.0F);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            int inside = 0;
            for (int sy = 0; sy < kSubSamples; ++sy) {
                for (int sx = 0; sx < kSubSamples; ++sx) {
                    const float u = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5F) / kSubSamples) / static_cast<float>(size);
                    const float v = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5F) / kSubSamples) / static_cast<float>(size);
                    if (InsidePolygon(kArrowShape, kArrowPointCount, u, v)) ++inside;
                }
            }
            constexpr float total = static_cast<float>(kSubSamples * kSubSamples);
            coverage[static_cast<std::size_t>(py) * size + px] = static_cast<float>(inside) / total;
        }
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top-down, matching the coverage grid
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000U;
    header.bV5GreenMask = 0x0000FF00U;
    header.bV5BlueMask = 0x000000FFU;
    header.bV5AlphaMask = 0xFF000000U;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    if (screen) ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    auto* pixels = static_cast<std::uint8_t*>(bits);
    std::memset(pixels, 0, pixelCount * 4);

    // Body: near-white, very slightly translucent and shaded top-to-bottom. The contour below lives
    // *outside* the silhouette rather than inside it: an inner outline eats the thin tail at 32px and
    // the pointer ends up looking hollow on a cream background, which is exactly the background this
    // app uses.
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            const auto index = static_cast<std::size_t>(py) * size + px;
            std::uint8_t* pixel = pixels + index * 4;
            if (coverage[index] > 0.0F) {
                const float t = static_cast<float>(py) / static_cast<float>(std::max(1, size - 1));
                StorePixel(pixel, kWhite, coverage[index] * (0.98F - 0.06F * t));
                continue;
            }
            int nearest = kShadowRadius + 1;
            for (int dy = -kShadowRadius; dy <= kShadowRadius && nearest > 1; ++dy) {
                for (int dx = -kShadowRadius; dx <= kShadowRadius; ++dx) {
                    const int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                    if (coverage[static_cast<std::size_t>(ny) * size + nx] <= 0.0F) continue;
                    nearest = std::min(nearest, std::max(std::abs(dx), std::abs(dy)));
                }
            }
            if (nearest == 1) StorePixel(pixel, kContour, 0.78F);
            else if (nearest == 2) StorePixel(pixel, kContour, 0.15F);
            else if (nearest == 3) StorePixel(pixel, kContour, 0.04F);
        }
    }

    const std::size_t maskStride = ((static_cast<std::size_t>(size) + 15) / 16) * 2;
    std::vector<std::uint8_t> maskBits(maskStride * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(std::lround(0.05F * size));
    info.yHotspot = static_cast<DWORD>(std::lround(0.02F * size));
    info.hbmMask = mask;
    info.hbmColor = color;
    HCURSOR cursor = CreateIconIndirect(&info);
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return cursor;
}

}  // namespace

HCURSOR Arrow(UINT dpi, D2D1_COLOR_F accent) noexcept {
    const UINT effectiveDpi = dpi != 0 ? dpi : 96U;
    const DWORD key = AccentKey(accent);
    for (const auto& entry : g_cache) {
        if (entry.cursor != nullptr && entry.dpi == effectiveDpi && entry.accentKey == key) return entry.cursor;
    }
    const int size = std::clamp(static_cast<int>(std::lround(32.0 * static_cast<double>(effectiveDpi) / 96.0)), 16, 128);
    HCURSOR cursor = BuildArrow(size, accent);
    if (cursor == nullptr) return nullptr;
    CacheEntry& slot = g_cache[g_nextSlot];
    if (slot.cursor != nullptr) DestroyCursor(slot.cursor);
    slot = CacheEntry{effectiveDpi, key, cursor};
    g_nextSlot = (g_nextSlot + 1) % kCacheSize;
    return cursor;
}

void Destroy() noexcept {
    for (auto& entry : g_cache) {
        if (entry.cursor != nullptr) DestroyCursor(entry.cursor);
        entry = CacheEntry{};
    }
    g_nextSlot = 0;
}

}  // namespace todo::CursorFactory
