#pragma once

// Clipboard image -> PNG bytes -> Direct2D bitmap.
//
// This module is deliberately independent of the task/domain model, the database and the window:
// it only knows Win32 clipboard formats, WIC and Direct2D. The caller decides what to do with the
// PNG bytes (persist them, show them, ...). Everything here is COM/GDI RAII and runs on the UI (STA)
// thread; OpenClipboard is always paired with CloseClipboard and no clipboard-owned handle outlives
// the read.
//
// Alpha convention: PNG files store *straight* (unassociated) alpha. A DIB on the clipboard has
// straight alpha too (or no alpha at all, in which case the 4th byte of a 32bpp BI_RGB row is
// undefined and is forced to opaque). The conversion to premultiplied BGRA happens exactly once,
// in CreateBitmapFromPng(), because the swap chain is DXGI_ALPHA_MODE_PREMULTIPLIED. Encoding a
// premultiplied buffer into a PNG would double-premultiply on the way back and darken every
// translucent pixel.
//
// v1.4 contract: the low-level codec below (ImageStatus / ImageResult / PngImage /
// ReadClipboardImage / CreateBitmapFromPng / RunClipboardImageSelfTest) is unchanged. On top of it
// sits a thin adapter, ReadClipboard(bool forceText), which is the single entry point for Ctrl+V:
// one OpenClipboard, image bytes + plain text in one snapshot, and AppError for the editor.
// Product limits live in todo:: (domain/BodyDocument.h); this header only defines codec-internal
// decoding safety valves, and deliberately defines NO kMaxImageDimension to avoid shadowing it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include "common/Error.h"
#include "domain/BodyDocument.h"

namespace todo::ui {

// "Empty" is a normal state, not an error: the clipboard simply has no image (or the caller passed
// an empty buffer). TooLarge and DecodeFailed mean an image *was* present.
enum class ImageStatus {
    Empty,
    Ok,
    TooLarge,
    DecodeFailed
};

// Codec-internal decoding safety valves. These are NOT product limits and may never reject a frame
// the product caps would accept: the authoritative caps are todo::kMaxImageBytes (4 MiB),
// todo::kMaxImageDimension (8192) and todo::kMaxImagePixels (32M) from domain/BodyDocument.h.
// There is intentionally no todo::ui::kMaxImageDimension: a same-named constant in this inner
// namespace would silently shadow todo::kMaxImageDimension for unqualified lookups.
constexpr std::size_t kCodecMaxRawBytes = 20u * 1024u * 1024u;  // baseline clipboard-block guard
// Derived from the product pixel cap: 32M pixels * 4 bytes = 128 MiB.
constexpr std::size_t kCodecMaxDecodedBytes = static_cast<std::size_t>(todo::kMaxImagePixels) * 4u;
static_assert(kCodecMaxDecodedBytes >= todo::kMaxImagePixels * 4u,
              "The decoded safety valve must not be stricter than the product pixel cap.");

struct PngImage {
    std::vector<std::uint8_t> bytes;
    std::uint32_t width{0};
    std::uint32_t height{0};
};

template <typename T>
struct ImageResult {
    ImageStatus status{ImageStatus::Empty};
    T value{};
    // Human-readable, UTF-8, safe to show in a toast: what failed plus the HRESULT / Win32 code.
    // Empty for Ok and Empty. Never contains the image bytes.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return status == ImageStatus::Ok; }
    [[nodiscard]] bool empty() const noexcept { return status == ImageStatus::Empty; }
};

using ClipboardImageResult = ImageResult<PngImage>;
using D2dBitmapResult = ImageResult<Microsoft::WRL::ComPtr<ID2D1Bitmap>>;

// Reads the first image the clipboard offers, in this order:
//   registered "PNG" -> registered "image/png" -> CF_DIBV5 -> CF_DIB -> CF_BITMAP.
// Registered PNG bytes are validated with WIC and returned verbatim (no re-encode). DIBs are
// normalised to 32bpp straight BGRA and encoded to PNG. CF_BITMAP is converted with GetDIBits.
// The clipboard is closed before any WIC work starts, so a slow encoder never holds the clipboard.
[[nodiscard]] ClipboardImageResult ReadClipboardImage();

// v1.4 adapter: the single entry point for one Ctrl+V. Opens the clipboard exactly once, reads the
// image (unless forceText, i.e. Ctrl+Shift+V) and the plain text (CF_UNICODETEXT, else CF_HTML with
// tags/entities stripped and no network access), then closes it before any WIC work. hasImage is
// false for an empty clipboard, for forceText, or for a rejected image; error is mapped from
// ImageStatus (TooLarge -> ImageTooLarge, DecodeFailed -> ImageInvalid). Never computes image_key.
struct ClipboardSnapshot {
    std::vector<std::uint8_t> png;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::wstring text;
    bool hasImage{false};
    AppError error;
};
[[nodiscard]] ClipboardSnapshot ReadClipboard(bool forceText);

// Decodes PNG bytes to GUID_WICPixelFormat32bppPBGRA and creates an ID2D1Bitmap from them, with a
// premultiplied B8G8R8A8 pixel format. The returned ID2D1Bitmap is usable with DrawBitmap.
[[nodiscard]] D2dBitmapResult CreateBitmapFromPng(ID2D1DeviceContext& context, const void* pngBytes, std::size_t size);
[[nodiscard]] inline D2dBitmapResult CreateBitmapFromPng(ID2D1DeviceContext& context, const std::vector<std::uint8_t>& png) {
    return CreateBitmapFromPng(context, png.data(), png.size());
}
[[nodiscard]] inline D2dBitmapResult CreateBitmapFromPng(ID2D1DeviceContext& context, const PngImage& png) {
    return CreateBitmapFromPng(context, png.bytes.data(), png.bytes.size());
}

// Fallback text for a toast when ImageResult::error is empty.
[[nodiscard]] const char* StatusText(ImageStatus status) noexcept;

// Window-less smoke test: builds a 32bpp bottom-up BI_RGB DIB (with deliberately zero alpha
// bytes), encodes it to PNG, decodes it back through WIC and checks the pixels. Passing a device
// context additionally exercises CreateBitmapFromPng(); nullptr keeps the test pure WIC, so it can
// run from a console process without D2D/D3D. On success `message` holds a short diagnostic.
[[nodiscard]] bool RunClipboardImageSelfTest(std::string& message) noexcept;
[[nodiscard]] bool RunClipboardImageSelfTest(ID2D1DeviceContext* context, std::string& message) noexcept;

}  // namespace todo::ui
