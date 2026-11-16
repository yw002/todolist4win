#include "ui/ClipboardImage.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace todo::ui {
namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// Small RAII helpers: clipboard ownership, GlobalLock and a screen DC.
// ---------------------------------------------------------------------------

class ClipboardGuard {
public:
    ClipboardGuard() noexcept {
        // Another process may still own the clipboard for a few milliseconds right after a paste.
        // Four bounded attempts keep that transient from turning into a spurious "no image".
        for (int attempt = 0; attempt < 4 && !opened_; ++attempt) {
            opened_ = OpenClipboard(nullptr) != FALSE;
            if (!opened_) {
                lastError_ = GetLastError();
                if (attempt + 1 < 4) Sleep(10);
            }
        }
    }
    ~ClipboardGuard() { if (opened_) CloseClipboard(); }
    ClipboardGuard(const ClipboardGuard&) = delete;
    ClipboardGuard& operator=(const ClipboardGuard&) = delete;

    [[nodiscard]] bool opened() const noexcept { return opened_; }
    [[nodiscard]] DWORD lastError() const noexcept { return lastError_; }

private:
    bool opened_{false};
    DWORD lastError_{ERROR_SUCCESS};
};

class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL handle) noexcept : handle_(handle) {
        if (!handle_) return;
        pointer_ = GlobalLock(handle_);
        if (pointer_) size_ = GlobalSize(handle_);
    }
    ~GlobalLockGuard() { if (pointer_) GlobalUnlock(handle_); }
    GlobalLockGuard(const GlobalLockGuard&) = delete;
    GlobalLockGuard& operator=(const GlobalLockGuard&) = delete;

    [[nodiscard]] void* data() const noexcept { return pointer_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    HGLOBAL handle_{nullptr};
    void* pointer_{nullptr};
    std::size_t size_{0};
};

class ScreenDcGuard {
public:
    ScreenDcGuard() noexcept : dc_(GetDC(nullptr)) {}
    ~ScreenDcGuard() { if (dc_) ReleaseDC(nullptr, dc_); }
    ScreenDcGuard(const ScreenDcGuard&) = delete;
    ScreenDcGuard& operator=(const ScreenDcGuard&) = delete;

    [[nodiscard]] HDC get() const noexcept { return dc_; }

private:
    HDC dc_{nullptr};
};

// ---------------------------------------------------------------------------
// Error formatting and result construction.
// ---------------------------------------------------------------------------

std::string HresultDetail(const char* operation, HRESULT hr) {
    char code[48]{};
    std::snprintf(code, sizeof(code), " (HRESULT 0x%08X)", static_cast<unsigned>(static_cast<std::uint32_t>(hr)));
    return std::string(operation) + code;
}

std::string Win32Detail(const char* operation, DWORD error) {
    char code[48]{};
    std::snprintf(code, sizeof(code), " (Win32 %lu)", static_cast<unsigned long>(error));
    return std::string(operation) + code;
}

template <typename T>
ImageResult<T> Failure(ImageStatus status, std::string message) {
    ImageResult<T> result;
    result.status = status;
    result.error = std::move(message);
    return result;
}

template <typename T>
ImageResult<T> Success(T value) {
    ImageResult<T> result;
    result.status = ImageStatus::Ok;
    result.value = std::move(value);
    return result;
}

HRESULT CreateWicFactory(ComPtr<IWICImagingFactory>& factory) {
    // A per-call factory instead of a process-wide static: CoUninitialize() runs before static
    // destructors in main.cpp, so a cached COM interface released at exit would be a shutdown-order
    // bug. CoCreateInstance of the WIC factory is cheap relative to encoding an image.
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(factory.GetAddressOf()));
}

// Product caps, single-sourced from domain/BodyDocument.h. Applies to both the PNG/IHDR path and
// the DIB path, before any WIC work or large allocation.
ImageStatus CheckFrameLimits(std::uint32_t width, std::uint32_t height, std::string& error) {
    if (width == 0 || height == 0) { error = "图片宽高为 0"; return ImageStatus::DecodeFailed; }
    if (width > todo::kMaxImageDimension || height > todo::kMaxImageDimension) {
        error = "图片单边超过 " + std::to_string(todo::kMaxImageDimension) + " 像素（" +
                std::to_string(width) + "x" + std::to_string(height) + "）";
        return ImageStatus::TooLarge;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > todo::kMaxImagePixels) {
        error = "图片总像素超过 " + std::to_string(todo::kMaxImagePixels) + "（" +
                std::to_string(width) + "x" + std::to_string(height) + "）";
        return ImageStatus::TooLarge;
    }
    return ImageStatus::Ok;
}

// IHDR decompression-bomb gate: must run BEFORE CreateDecoderFromStream. WIC rejects malformed
// headers but happily allocates for a legal IHDR that declares a huge frame with almost no data.
// PNG layout: 8-byte signature, 4-byte IHDR length, "IHDR", width(4 BE) at 16, height(4 BE) at 20.
ImageStatus CheckPngIhdr(const std::uint8_t* data, std::size_t size,
                         std::uint32_t& width, std::uint32_t& height, std::string& error) {
    static constexpr std::uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || size < sizeof(kPngSignature) ||
        std::memcmp(data, kPngSignature, sizeof(kPngSignature)) != 0) {
        error = "剪贴板中的 PNG 数据签名无效";
        return ImageStatus::DecodeFailed;
    }
    if (size < 24 || std::memcmp(data + 12, "IHDR", 4) != 0) {
        error = "剪贴板中的 PNG 缺少 IHDR";
        return ImageStatus::DecodeFailed;
    }
    const auto bigEndian = [data](std::size_t offset) -> std::uint32_t {
        return (static_cast<std::uint32_t>(data[offset]) << 24) |
               (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
               static_cast<std::uint32_t>(data[offset + 3]);
    };
    width = bigEndian(16);
    height = bigEndian(20);
    const ImageStatus limits = CheckFrameLimits(width, height, error);
    if (limits != ImageStatus::Ok) return limits;
    if (size > todo::kMaxImageBytes) {
        error = "图片超过 " + std::to_string(todo::kMaxImageBytes / (1024u * 1024u)) + " MB，请压缩后再粘贴";
        return ImageStatus::TooLarge;
    }
    return ImageStatus::Ok;
}

HRESULT StreamSize(IStream& stream, std::uint64_t& size) {
    LARGE_INTEGER zero{};
    ULARGE_INTEGER end{};
    const HRESULT hr = stream.Seek(zero, STREAM_SEEK_END, &end);
    if (FAILED(hr)) return hr;
    size = end.QuadPart;
    return S_OK;
}

HRESULT ReadStream(IStream& stream, std::vector<std::uint8_t>& out) {
    LARGE_INTEGER zero{};
    HRESULT hr = stream.Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    std::size_t done = 0;
    while (done < out.size()) {
        const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(out.size() - done, 1u << 20));
        ULONG read = 0;
        hr = stream.Read(out.data() + done, chunk, &read);
        if (FAILED(hr)) return hr;
        if (read == 0) return E_FAIL;
        done += read;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// DIB decoding. The clipboard gives us a packed, usually bottom-up, 4-byte-aligned
// bitmap; WIC needs a flat 32bpp straight-BGRA buffer. Palettes, 16/32bpp
// bitfields and BITMAPV4/V5 alpha masks are all normalised here.
// ---------------------------------------------------------------------------

struct DibLayout {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::uint32_t bpp{0};
    std::uint32_t compression{BI_RGB};
    bool topDown{false};
    std::uint32_t masks[4]{0, 0, 0, 0};  // R, G, B, A
    bool hasAlphaMask{false};
    std::uint32_t paletteCount{0};
    const RGBQUAD* palette{nullptr};
    const std::uint8_t* pixels{nullptr};
    std::size_t pixelBytes{0};
};

struct MaskInfo {
    std::uint32_t mask{0};
    std::uint32_t shift{0};
    std::uint32_t maximum{0};
};

MaskInfo MakeMaskInfo(std::uint32_t mask) noexcept {
    MaskInfo info;
    info.mask = mask;
    if (!mask) return info;
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if ((mask & (1u << bit)) != 0) { info.shift = bit; break; }
    }
    info.maximum = mask >> info.shift;
    return info;
}

std::uint8_t DecodeChannel(std::uint32_t pixel, const MaskInfo& info) noexcept {
    if (!info.mask || !info.maximum) return 0;
    const std::uint32_t raw = (pixel & info.mask) >> info.shift;
    const std::uint64_t scaled = (static_cast<std::uint64_t>(raw) * 255u + info.maximum / 2u) / info.maximum;
    return static_cast<std::uint8_t>(std::min<std::uint64_t>(scaled, 255u));
}

bool PeekDibDimensions(const std::uint8_t* data, std::size_t size, std::uint32_t& width, std::uint32_t& height) {
    if (!data || size < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER info{};
    std::memcpy(&info, data, sizeof(info));
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biSize > size) return false;
    const std::int32_t signedHeight = static_cast<std::int32_t>(info.biHeight);
    if (info.biWidth <= 0 || signedHeight == 0 || signedHeight == (std::numeric_limits<std::int32_t>::min)()) return false;
    width = static_cast<std::uint32_t>(info.biWidth);
    height = static_cast<std::uint32_t>(signedHeight < 0
                                            ? -static_cast<std::int64_t>(signedHeight)
                                            : static_cast<std::int64_t>(signedHeight));
    return true;
}

bool ParseDib(const std::uint8_t* data, std::size_t size, DibLayout& out, std::string& error) {
    if (!data || size < sizeof(BITMAPINFOHEADER)) { error = "剪贴板图像数据不完整（DIB 头被截断）"; return false; }
    BITMAPINFOHEADER info{};
    std::memcpy(&info, data, sizeof(info));
    const std::uint32_t headerSize = info.biSize;
    if (headerSize < sizeof(BITMAPINFOHEADER) || headerSize > size) { error = "剪贴板图像使用了无法识别的 DIB 头"; return false; }

    const std::int32_t signedHeight = static_cast<std::int32_t>(info.biHeight);
    out.width = static_cast<std::uint32_t>(info.biWidth);
    out.height = static_cast<std::uint32_t>(signedHeight < 0 ? -static_cast<std::int64_t>(signedHeight) : signedHeight);
    out.topDown = signedHeight < 0;
    out.bpp = info.biBitCount;
    out.compression = info.biCompression;
    out.stride = static_cast<std::uint32_t>(((static_cast<std::uint64_t>(out.width) * out.bpp + 31u) / 32u) * 4u);

    if (out.bpp != 1 && out.bpp != 4 && out.bpp != 8 && out.bpp != 16 && out.bpp != 24 && out.bpp != 32) {
        error = "不支持的位图色深（仅支持 1/4/8/16/24/32bpp）";
        return false;
    }
    const bool bitfields = out.compression == BI_BITFIELDS
#ifdef BI_ALPHABITFIELDS
                           || out.compression == BI_ALPHABITFIELDS
#endif
        ;
    if (out.compression != BI_RGB && !bitfields) {
        error = "不支持压缩的 DIB（RLE/JPEG/PNG 压缩位图）";
        return false;
    }
    if (bitfields && out.bpp != 16 && out.bpp != 32) {
        error = "BI_BITFIELDS 只能用于 16/32bpp DIB";
        return false;
    }

    std::size_t cursor = headerSize;
    if (bitfields && headerSize >= sizeof(BITMAPV4HEADER)) {
        // BITMAPV4/V5 carry the channel masks inside the header. V4 and V5 share the same layout up
        // to bV4AlphaMask, so reading the smaller struct from either header is safe.
        BITMAPV4HEADER v4{};
        std::memcpy(&v4, data, sizeof(v4));
        out.masks[0] = v4.bV4RedMask;
        out.masks[1] = v4.bV4GreenMask;
        out.masks[2] = v4.bV4BlueMask;
        out.masks[3] = v4.bV4AlphaMask;
        out.hasAlphaMask = v4.bV4AlphaMask != 0;
    } else if (bitfields) {
        // A 40-byte BITMAPINFOHEADER stores 3 (BI_BITFIELDS) or 4 (BI_ALPHABITFIELDS) DWORD masks
        // immediately after the header, before any pixel data.
        const std::uint32_t maskCount =
#ifdef BI_ALPHABITFIELDS
            out.compression == BI_ALPHABITFIELDS ? 4u : 3u;
#else
            3u;
#endif
        if (cursor + static_cast<std::size_t>(maskCount) * sizeof(DWORD) > size) {
            error = "剪贴板图像的通道掩码被截断";
            return false;
        }
        for (std::uint32_t i = 0; i < maskCount; ++i) {
            std::memcpy(&out.masks[i], data + cursor + static_cast<std::size_t>(i) * sizeof(DWORD), sizeof(DWORD));
        }
        out.hasAlphaMask = maskCount == 4 && out.masks[3] != 0;
        cursor += static_cast<std::size_t>(maskCount) * sizeof(DWORD);
    } else if (out.bpp == 16) {
        // BI_RGB 16bpp is implicitly RGB555.
        out.masks[0] = 0x7C00;
        out.masks[1] = 0x03E0;
        out.masks[2] = 0x001F;
    }
    if (!bitfields && headerSize >= sizeof(BITMAPV4HEADER)) {
        // Some producers set BI_RGB on a V4/V5 header but still declare an alpha mask and store
        // real alpha in the 4th byte. Honour the declared mask; with no mask that byte is undefined
        // (GDI screenshots fill it with 0) and must be forced opaque.
        BITMAPV4HEADER v4{};
        std::memcpy(&v4, data, sizeof(v4));
        out.masks[3] = v4.bV4AlphaMask;
        out.hasAlphaMask = v4.bV4AlphaMask != 0;
    }

    if (out.bpp <= 8) {
        out.paletteCount = info.biClrUsed != 0 ? info.biClrUsed : (1u << out.bpp);
        const std::uint64_t tableBytes = static_cast<std::uint64_t>(out.paletteCount) * sizeof(RGBQUAD);
        if (cursor + tableBytes > size) { error = "剪贴板图像的调色板被截断"; return false; }
        out.palette = reinterpret_cast<const RGBQUAD*>(data + cursor);
        cursor += static_cast<std::size_t>(tableBytes);
    }

    if (headerSize >= sizeof(BITMAPV5HEADER)) {
        BITMAPV5HEADER v5{};
        std::memcpy(&v5, data, sizeof(v5));
        // wingdi.h names these PROFILE_EMBEDDED / PROFILE_LINKED, and older SDKs omit them. The
        // documented FOURCC values keep this independent of the SDK version.
        constexpr std::uint32_t kProfileEmbedded = 0x4D424544u;  // 'MBED'
        constexpr std::uint32_t kProfileLinked = 0x4C494E4Bu;    // 'LINK'
        if (v5.bV5CSType == kProfileEmbedded || v5.bV5CSType == kProfileLinked) {
            // bV5ProfileData is an offset from the start of the header, and the profile sits
            // between the color table and the bits. Skipping it is required or the pixels shift.
            const std::uint64_t profileEnd = static_cast<std::uint64_t>(v5.bV5ProfileData) + v5.bV5ProfileSize;
            if (profileEnd > size) { error = "剪贴板图像的色彩配置数据被截断"; return false; }
            cursor = std::max<std::size_t>(cursor, static_cast<std::size_t>(profileEnd));
        }
    }

    const std::uint64_t needed = static_cast<std::uint64_t>(out.stride) * out.height;
    if (cursor > size || needed > static_cast<std::uint64_t>(size - cursor)) {
        error = "剪贴板图像的像素数据被截断";
        return false;
    }
    out.pixels = data + cursor;
    out.pixelBytes = size - cursor;
    return true;
}

void NormaliseDibToBgra(const DibLayout& layout, std::vector<std::uint8_t>& out) {
    const MaskInfo red = MakeMaskInfo(layout.masks[0]);
    const MaskInfo green = MakeMaskInfo(layout.masks[1]);
    const MaskInfo blue = MakeMaskInfo(layout.masks[2]);
    const MaskInfo alpha = MakeMaskInfo(layout.masks[3]);
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        // DIB rows are bottom-up unless biHeight is negative (top-down).
        const std::uint32_t sourceRow = layout.topDown ? y : (layout.height - 1u - y);
        const std::uint8_t* row = layout.pixels + static_cast<std::size_t>(sourceRow) * layout.stride;
        for (std::uint32_t x = 0; x < layout.width; ++x) {
            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            std::uint8_t a = 255;
            if (layout.bpp == 32) {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 4u, sizeof(pixel));
                if (layout.compression == BI_RGB) {
                    // BI_RGB 32bpp leaves the 4th byte undefined; GDI screenshots fill it with 0,
                    // which would make the whole image transparent if it were treated as alpha.
                    // Only a V4/V5 alpha mask makes that byte meaningful.
                    b = row[static_cast<std::size_t>(x) * 4u];
                    g = row[static_cast<std::size_t>(x) * 4u + 1u];
                    r = row[static_cast<std::size_t>(x) * 4u + 2u];
                    a = layout.hasAlphaMask ? row[static_cast<std::size_t>(x) * 4u + 3u] : 255;
                } else {
                    b = DecodeChannel(pixel, blue);
                    g = DecodeChannel(pixel, green);
                    r = DecodeChannel(pixel, red);
                    a = layout.hasAlphaMask ? DecodeChannel(pixel, alpha) : 255;
                }
            } else if (layout.bpp == 24) {
                const std::uint8_t* source = row + static_cast<std::size_t>(x) * 3u;
                b = source[0];
                g = source[1];
                r = source[2];
            } else if (layout.bpp == 16) {
                std::uint16_t pixel = 0;
                std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 2u, sizeof(pixel));
                b = DecodeChannel(pixel, blue);
                g = DecodeChannel(pixel, green);
                r = DecodeChannel(pixel, red);
            } else {
                // 1/4/8bpp palettised DIB: packed indices, most significant bits first.
                std::uint32_t index = 0;
                if (layout.bpp == 8) index = row[x];
                else if (layout.bpp == 4) index = (x & 1u) == 0 ? (row[x / 2u] >> 4) : (row[x / 2u] & 0x0Fu);
                else index = (row[x / 8u] >> (7u - (x & 7u))) & 1u;
                if (layout.palette && index < layout.paletteCount) {
                    const RGBQUAD& entry = layout.palette[index];
                    b = entry.rgbBlue;
                    g = entry.rgbGreen;
                    r = entry.rgbRed;
                }
            }
            std::uint8_t* target = out.data() + (static_cast<std::size_t>(y) * layout.width + x) * 4u;
            target[0] = b;
            target[1] = g;
            target[2] = r;
            target[3] = a;
        }
    }
}

// ---------------------------------------------------------------------------
// WIC encode / decode.
// ---------------------------------------------------------------------------

HRESULT EncodeBgraToPng(IWICImagingFactory& factory, std::vector<std::uint8_t>& bgra,
                        std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& png) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!stream) return E_OUTOFMEMORY;

    const std::uint32_t stride = width * 4u;
    ComPtr<IWICBitmap> source;
    HRESULT hr = factory.CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, stride,
                                                static_cast<UINT>(bgra.size()), bgra.data(), source.GetAddressOf());
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory.CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return hr;
    hr = frame->WriteSource(source.Get(), nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->Commit();
    if (FAILED(hr)) return hr;
    hr = encoder->Commit();
    if (FAILED(hr)) return hr;

    std::uint64_t size = 0;
    hr = StreamSize(*stream.Get(), size);
    if (FAILED(hr)) return hr;
    if (size > todo::kMaxImageBytes) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    png.resize(static_cast<std::size_t>(size));
    return ReadStream(*stream.Get(), png);
}

HRESULT OpenPngFrame(IWICImagingFactory& factory, const std::uint8_t* data, std::size_t size,
                     ComPtr<IWICBitmapFrameDecode>& frame) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
    if (!stream) return E_OUTOFMEMORY;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf());
    if (FAILED(hr)) return hr;
    return decoder->GetFrame(0, frame.GetAddressOf());
}

HRESULT DecodePngToBgra(IWICImagingFactory& factory, const std::vector<std::uint8_t>& png,
                        std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& bgra) {
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = OpenPngFrame(factory, png.data(), png.size(), frame);
    if (FAILED(hr)) return hr;
    UINT w = 0;
    UINT h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) return hr;
    width = w;
    height = h;
    ComPtr<IWICFormatConverter> converter;
    hr = factory.CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) return hr;
    const std::size_t stride = static_cast<std::size_t>(w) * 4u;
    bgra.assign(stride * h, 0);
    return converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(bgra.size()), bgra.data());
}

// ---------------------------------------------------------------------------
// Clipboard payload extraction. All Win32 clipboard work happens inside the
// ClipboardGuard scope; only owned copies are used after it closes.
// ---------------------------------------------------------------------------

enum class ClipboardKind { None, Png, DibV5, Dib, Bitmap };

struct ClipboardPayload {
    ClipboardKind kind{ClipboardKind::None};
    std::vector<std::uint8_t> bytes;
    std::string formatName;
};

// ---------------------------------------------------------------------------
// Text sources for the v1.4 adapter. Everything here works on copies taken
// inside the single ClipboardGuard; nothing dereferences a URL. CF_HTML is
// used ONLY as a plain-text fallback in the MVP (data:image;base64 -> image is
// explicitly P2); remote URLs and file:// links are never fetched or resolved.
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxClipboardTextChars = 65536;
constexpr std::size_t kMaxHtmlBytes = 1024u * 1024u;

bool ReadUnicodeText(HANDLE handle, std::wstring& out) {
    GlobalLockGuard lock(static_cast<HGLOBAL>(handle));
    if (!lock.data()) return false;
    const std::size_t chars = lock.size() / sizeof(wchar_t);
    const wchar_t* begin = static_cast<const wchar_t*>(lock.data());
    std::size_t length = 0;
    while (length < chars && length < kMaxClipboardTextChars && begin[length] != L'\0') ++length;
    if (length == 0) return false;
    out.assign(begin, begin + length);
    return true;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::wstring LowerAscii(std::wstring value) {
    for (wchar_t& character : value) {
        if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character - L'A' + L'a');
    }
    return value;
}

// Returns 0 when the entity is unknown, so the caller leaves the original text untouched.
wchar_t DecodeHtmlEntity(const std::wstring& entity) {
    if (entity == L"amp") return L'&';
    if (entity == L"lt") return L'<';
    if (entity == L"gt") return L'>';
    if (entity == L"quot") return L'"';
    if (entity == L"apos" || entity == L"#39") return L'\'';
    if (entity == L"nbsp") return L' ';
    if (entity.size() < 2 || entity[0] != L'#') return 0;
    const bool hex = entity.size() > 2 && (entity[1] == L'x' || entity[1] == L'X');
    const std::size_t first = hex ? 2u : 1u;
    unsigned long value = 0;
    for (std::size_t i = first; i < entity.size(); ++i) {
        const wchar_t character = entity[i];
        unsigned digit = 0;
        if (character >= L'0' && character <= L'9') digit = static_cast<unsigned>(character - L'0');
        else if (hex && character >= L'a' && character <= L'f') digit = static_cast<unsigned>(character - L'a') + 10u;
        else if (hex && character >= L'A' && character <= L'F') digit = static_cast<unsigned>(character - L'A') + 10u;
        else return 0;
        value = value * (hex ? 16ul : 10ul) + digit;
        if (value > 0xFFFFul) return 0;  // keep to the BMP; no surrogate pairs from a clipboard paste
    }
    return value == 0 ? 0 : static_cast<wchar_t>(value);
}

std::wstring StripHtml(const std::wstring& input) {
    std::wstring out;
    bool skipping = false;  // inside <script> / <style>
    for (std::size_t i = 0; i < input.size() && out.size() < kMaxClipboardTextChars;) {
        if (input[i] == L'<') {
            const std::size_t close = input.find(L'>', i);
            if (close == std::wstring::npos) break;
            std::wstring name = input.substr(i + 1, close - i - 1);
            const std::size_t space = name.find_first_of(L" \t\r\n/");
            if (space != std::wstring::npos) name.resize(space);
            name = LowerAscii(std::move(name));
            if (name == L"script" || name == L"style") skipping = true;
            else if (name == L"/script" || name == L"/style") skipping = false;
            else if (!skipping && (name == L"br" || name == L"/p" || name == L"/div" || name == L"/li" || name == L"/tr")) {
                out.push_back(L'\n');
            }
            i = close + 1;
            continue;
        }
        if (skipping) { ++i; continue; }
        if (input[i] == L'&') {
            const std::size_t semi = input.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 12) {
                const wchar_t decoded = DecodeHtmlEntity(input.substr(i + 1, semi - i - 1));
                if (decoded != 0) { out.push_back(decoded); i = semi + 1; continue; }
            }
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

std::optional<std::size_t> HtmlFragmentOffset(const std::string& data, const char* key) {
    const std::size_t found = data.find(key);
    if (found == std::string::npos) return std::nullopt;
    std::size_t cursor = found + std::strlen(key);
    while (cursor < data.size() && (data[cursor] == ' ' || data[cursor] == '\t')) ++cursor;
    std::size_t value = 0;
    std::size_t digits = 0;
    while (cursor < data.size() && data[cursor] >= '0' && data[cursor] <= '9') {
        value = value * 10u + static_cast<std::size_t>(data[cursor] - '0');
        ++cursor;
        ++digits;
    }
    if (digits == 0 || value > data.size()) return std::nullopt;
    return value;
}

bool ReadHtmlText(HANDLE handle, std::wstring& out) {
    GlobalLockGuard lock(static_cast<HGLOBAL>(handle));
    if (!lock.data()) return false;
    const char* bytes = static_cast<const char*>(lock.data());
    const std::size_t size = std::min<std::size_t>(lock.size(), kMaxHtmlBytes);
    const std::string data(bytes, bytes + size);
    // CF_HTML carries a text header with byte offsets into the same buffer. Prefer the declared
    // fragment; fall back to everything from the first tag when a producer omitted the header.
    const auto start = HtmlFragmentOffset(data, "StartFragment:");
    const auto end = HtmlFragmentOffset(data, "EndFragment:");
    std::string fragment;
    if (start && end && *end > *start) {
        fragment = data.substr(*start, *end - *start);
    } else {
        const std::size_t firstTag = data.find('<');
        fragment = firstTag == std::string::npos ? data : data.substr(firstTag);
    }
    out = StripHtml(Utf8ToWide(fragment));
    return !out.empty();
}

bool CopyClipboardBytes(HANDLE handle, ClipboardKind kind, std::vector<std::uint8_t>& out,
                        std::size_t& rawSize, ImageStatus& failureStatus, std::string& error) {
    failureStatus = ImageStatus::DecodeFailed;
    GlobalLockGuard lock(static_cast<HGLOBAL>(handle));
    if (!lock.data()) { error = Win32Detail("无法锁定剪贴板数据", GetLastError()); return false; }
    rawSize = lock.size();
    if (rawSize == 0) { error = "剪贴板图像数据为空"; return false; }
    const std::uint8_t* source = static_cast<const std::uint8_t*>(lock.data());
    // Effective ceiling. A registered PNG is a final artefact, so the product byte cap applies
    // directly (and avoids copying 20 MiB we would reject anyway). For DIBs the 20 MiB codec valve
    // is raised to at least the decoded 32bpp size of the declared frame: otherwise a perfectly
    // legal 4096x4096 screenshot (67 MiB of DIB, but a small PNG) would be rejected by an internal
    // valve before the product limits ever got a say. "Not stricter than the product caps" is a
    // hard requirement of the v1.4 ruling.
    std::size_t cap = kind == ClipboardKind::Png ? todo::kMaxImageBytes : kCodecMaxRawBytes;
    if (kind == ClipboardKind::DibV5 || kind == ClipboardKind::Dib) {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (PeekDibDimensions(source, rawSize, width, height)) {
            const std::uint64_t declared = static_cast<std::uint64_t>(width) * height;
            if (declared <= todo::kMaxImagePixels) {
                const std::size_t needed = static_cast<std::size_t>(declared) * 4u;
                cap = std::max(cap, needed + sizeof(BITMAPV5HEADER) + 65536u);
            }
        }
    }
    if (rawSize > cap) {
        failureStatus = ImageStatus::TooLarge;
        error = kind == ClipboardKind::Png
                    ? "图片超过 " + std::to_string(todo::kMaxImageBytes / (1024u * 1024u)) + " MB，请压缩后再粘贴"
                    : "剪贴板图像原始数据超过解码安全阀上限";
        return false;
    }
    out.assign(source, source + rawSize);
    return true;
}

// CF_BITMAP is a GDI handle (no alpha, not a global memory block): turn it into a 32bpp DIB with
// GetDIBits so the very same normaliser/encoder path can be reused.
bool ConvertBitmapToDib(HBITMAP bitmap, std::vector<std::uint8_t>& dib,
                        ImageStatus& failureStatus, std::string& error) {
    failureStatus = ImageStatus::DecodeFailed;
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0) {
        error = Win32Detail("无法读取剪贴板位图尺寸", GetLastError());
        return false;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(info.bmWidth);
    const std::uint32_t height = static_cast<std::uint32_t>(info.bmHeight);
    const ImageStatus limits = CheckFrameLimits(width, height, error);
    if (limits != ImageStatus::Ok) { failureStatus = limits; return false; }
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * 32u + 31u) / 32u) * 4u;
    const std::uint64_t pixelBytes = stride * height;
    if (pixelBytes > kCodecMaxDecodedBytes) {
        failureStatus = ImageStatus::TooLarge;
        error = "剪贴板位图解码后超过安全缓冲上限";
        return false;
    }
    dib.assign(sizeof(BITMAPINFOHEADER) + static_cast<std::size_t>(pixelBytes), 0);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(width);
    header.biHeight = static_cast<LONG>(height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(pixelBytes);
    std::memcpy(dib.data(), &header, sizeof(header));
    BITMAPINFO request{};
    request.bmiHeader = header;
    ScreenDcGuard dc;
    if (!dc.get()) { error = "无法获取屏幕 DC"; return false; }
    const int rows = GetDIBits(dc.get(), bitmap, 0, height, dib.data() + sizeof(BITMAPINFOHEADER),
                               &request, DIB_RGB_COLORS);
    if (rows != static_cast<int>(height)) {
        error = Win32Detail("GetDIBits 无法读取完整的位图行", GetLastError());
        return false;
    }
    return true;
}

// One clipboard open per Ctrl+V. Both public entry points go through here: the image bytes and the
// plain text are copied into owned buffers inside the SAME ClipboardGuard, which is destroyed when
// this function returns - before any WIC encoding/validation starts. forceText (Ctrl+Shift+V)
// skips every image candidate but still reads the text.
struct ClipboardRead {
    ClipboardPayload payload;
    std::wstring text;
    ImageStatus status{ImageStatus::Empty};
    std::string error;
};

ClipboardRead ReadClipboardPayload(bool forceText) {
    ClipboardRead result;
    ClipboardGuard clipboard;
    if (!clipboard.opened()) {
        result.status = ImageStatus::DecodeFailed;
        result.error = Win32Detail("无法打开剪贴板", clipboard.lastError());
        return result;
    }

    // Text first: it is cheap and independent of the image format priority. CF_HTML is only a
    // fallback when CF_UNICODETEXT is absent, so a copy from a browser is not pasted twice.
    if (HANDLE textHandle = GetClipboardData(CF_UNICODETEXT)) ReadUnicodeText(textHandle, result.text);
    if (result.text.empty()) {
        const UINT htmlFormat = RegisterClipboardFormatW(L"HTML Format");
        if (htmlFormat != 0) {
            if (HANDLE htmlHandle = GetClipboardData(htmlFormat)) ReadHtmlText(htmlHandle, result.text);
        }
    }
    if (forceText) return result;

    struct Candidate {
        UINT format;
        ClipboardKind kind;
        const char* name;
    };
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    const UINT imagePngFormat = RegisterClipboardFormatW(L"image/png");
    std::vector<Candidate> candidates;
    if (pngFormat != 0) candidates.push_back({pngFormat, ClipboardKind::Png, "PNG"});
    if (imagePngFormat != 0 && imagePngFormat != pngFormat) {
        candidates.push_back({imagePngFormat, ClipboardKind::Png, "image/png"});
    }
#ifdef CF_DIBV5
    candidates.push_back({CF_DIBV5, ClipboardKind::DibV5, "CF_DIBV5"});
#endif
    candidates.push_back({CF_DIB, ClipboardKind::Dib, "CF_DIB"});
    candidates.push_back({CF_BITMAP, ClipboardKind::Bitmap, "CF_BITMAP"});

    ImageStatus failureStatus = ImageStatus::Empty;
    std::string copyError;
    for (const Candidate& candidate : candidates) {
        if (!IsClipboardFormatAvailable(candidate.format)) continue;
        HANDLE handle = GetClipboardData(candidate.format);
        if (!handle) continue;
        result.payload.kind = candidate.kind;
        result.payload.formatName = candidate.name;
        if (candidate.kind == ClipboardKind::Bitmap) {
            // The HBITMAP belongs to the clipboard: copy its pixels before the guard closes and
            // never DeleteObject it.
            std::vector<std::uint8_t> dib;
            if (!ConvertBitmapToDib(static_cast<HBITMAP>(handle), dib, failureStatus, copyError)) {
                if (failureStatus == ImageStatus::TooLarge) {
                    result.status = ImageStatus::TooLarge;
                    result.error = std::move(copyError);
                    return result;
                }
                result.payload.kind = ClipboardKind::None;
                continue;
            }
            result.payload.bytes = std::move(dib);
            return result;
        }
        std::size_t rawSize = 0;
        if (!CopyClipboardBytes(handle, candidate.kind, result.payload.bytes, rawSize, failureStatus, copyError)) {
            if (failureStatus == ImageStatus::TooLarge) {
                result.status = ImageStatus::TooLarge;
                result.error = std::move(copyError);
                return result;
            }
            result.payload.kind = ClipboardKind::None;
            continue;
        }
        return result;
    }
    // No image at all, or a format claimed to have one but could not be read.
    if (!copyError.empty()) {
        result.status = ImageStatus::DecodeFailed;
        result.error = std::move(copyError);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Shared encode paths.
// ---------------------------------------------------------------------------

ClipboardImageResult EncodeDibBytes(const std::vector<std::uint8_t>& dib, const std::string& formatName) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!PeekDibDimensions(dib.data(), dib.size(), width, height)) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, formatName + "：DIB 头不完整或尺寸非法");
    }
    std::string limitError;
    const ImageStatus limits = CheckFrameLimits(width, height, limitError);
    if (limits != ImageStatus::Ok) return Failure<PngImage>(limits, limitError);
    const std::uint64_t decodedBytes = static_cast<std::uint64_t>(width) * height * 4u;
    if (decodedBytes > kCodecMaxDecodedBytes) {
        return Failure<PngImage>(ImageStatus::TooLarge, "剪贴板图像解码后超过安全缓冲上限");
    }

    DibLayout layout;
    std::string error;
    if (!ParseDib(dib.data(), dib.size(), layout, error)) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, formatName + "：" + error);
    }
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(decodedBytes), 0);
    NormaliseDibToBgra(layout, bgra);

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateWicFactory(factory);
    if (FAILED(hr)) return Failure<PngImage>(ImageStatus::DecodeFailed, HresultDetail("无法创建 WIC 工厂", hr));
    PngImage image;
    image.width = layout.width;
    image.height = layout.height;
    hr = EncodeBgraToPng(*factory.Get(), bgra, layout.width, layout.height, image.bytes);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)) {
        return Failure<PngImage>(ImageStatus::TooLarge,
                                 "图片超过 " + std::to_string(todo::kMaxImageBytes / (1024u * 1024u)) + " MB，请压缩后再粘贴");
    }
    if (FAILED(hr)) return Failure<PngImage>(ImageStatus::DecodeFailed, HresultDetail("PNG 编码失败", hr));
    if (image.bytes.empty()) return Failure<PngImage>(ImageStatus::DecodeFailed, "PNG 编码结果为空");
    return Success<PngImage>(std::move(image));
}

ClipboardImageResult ValidatePngBytes(const std::vector<std::uint8_t>& png) {
    std::uint32_t ihdrWidth = 0;
    std::uint32_t ihdrHeight = 0;
    std::string limitError;
    // Decompression-bomb gate first: a legal IHDR may declare a huge frame with almost no payload,
    // and WIC would start allocating for it before reporting anything we could act on.
    const ImageStatus gate = CheckPngIhdr(png.data(), png.size(), ihdrWidth, ihdrHeight, limitError);
    if (gate != ImageStatus::Ok) return Failure<PngImage>(gate, limitError);
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateWicFactory(factory);
    if (FAILED(hr)) return Failure<PngImage>(ImageStatus::DecodeFailed, HresultDetail("无法创建 WIC 工厂", hr));
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = OpenPngFrame(*factory.Get(), png.data(), png.size(), frame);
    if (FAILED(hr)) return Failure<PngImage>(ImageStatus::DecodeFailed, HresultDetail("PNG 解码失败", hr));
    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return Failure<PngImage>(ImageStatus::DecodeFailed, HresultDetail("无法读取 PNG 尺寸", hr));
    if (width != ihdrWidth || height != ihdrHeight) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, "PNG 的 IHDR 尺寸与解码帧尺寸不一致");
    }
    PngImage image;
    image.bytes = png;
    image.width = width;
    image.height = height;
    return Success<PngImage>(std::move(image));
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

ClipboardImageResult ReadClipboardImage() {
    try {
        // One OpenClipboard, all copies owned, guard already destroyed on return. WIC work only
        // starts here - never while the clipboard is open.
        const ClipboardRead read = ReadClipboardPayload(false);
        if (read.status == ImageStatus::TooLarge || read.status == ImageStatus::DecodeFailed) {
            return Failure<PngImage>(read.status, read.error);
        }
        if (read.payload.kind == ClipboardKind::None) return ClipboardImageResult{};
        if (read.payload.kind == ClipboardKind::Png) return ValidatePngBytes(read.payload.bytes);
        return EncodeDibBytes(read.payload.bytes, read.payload.formatName);
    } catch (const std::bad_alloc&) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, "内存不足，无法读取剪贴板图像");
    } catch (const std::exception& exception) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, std::string("读取剪贴板图像发生异常：") + exception.what());
    } catch (...) {
        return Failure<PngImage>(ImageStatus::DecodeFailed, "读取剪贴板图像发生未知异常");
    }
}

ClipboardSnapshot ReadClipboard(bool forceText) {
    ClipboardSnapshot snapshot;
    try {
        const ClipboardRead read = ReadClipboardPayload(forceText);
        snapshot.text = read.text;
        if (read.status == ImageStatus::TooLarge || read.status == ImageStatus::DecodeFailed) {
            snapshot.error = {read.status == ImageStatus::TooLarge ? ErrorCode::ImageTooLarge : ErrorCode::ImageInvalid,
                              read.error};
            return snapshot;
        }
        // forceText (Ctrl+Shift+V) must never produce an image, even when the clipboard has one.
        if (forceText || read.payload.kind == ClipboardKind::None) return snapshot;

        ClipboardImageResult image;
        if (read.payload.kind == ClipboardKind::Png) image = ValidatePngBytes(read.payload.bytes);
        else image = EncodeDibBytes(read.payload.bytes, read.payload.formatName);

        if (image.ok()) {
            snapshot.png = std::move(image.value.bytes);
            snapshot.width = image.value.width;
            snapshot.height = image.value.height;
            snapshot.hasImage = true;
            return snapshot;
        }
        // Empty stays a silent "no image"; the codec's TooLarge/DecodeFailed become AppError.
        if (image.status == ImageStatus::Empty) return snapshot;
        snapshot.error = {image.status == ImageStatus::TooLarge ? ErrorCode::ImageTooLarge : ErrorCode::ImageInvalid,
                          image.error.empty() ? StatusText(image.status) : image.error};
        return snapshot;
    } catch (const std::bad_alloc&) {
        snapshot.error = {ErrorCode::ImageInvalid, "内存不足，无法读取剪贴板内容"};
    } catch (const std::exception& exception) {
        snapshot.error = {ErrorCode::ImageInvalid, std::string("读取剪贴板内容发生异常：") + exception.what()};
    } catch (...) {
        snapshot.error = {ErrorCode::ImageInvalid, "读取剪贴板内容发生未知异常"};
    }
    snapshot.hasImage = false;
    snapshot.png.clear();
    return snapshot;
}

D2dBitmapResult CreateBitmapFromPng(ID2D1DeviceContext& context, const void* pngBytes, std::size_t size) {
    try {
        if (!pngBytes || size == 0) return D2dBitmapResult{};
        if (size > todo::kMaxImageBytes) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(
                ImageStatus::TooLarge,
                "图片超过 " + std::to_string(todo::kMaxImageBytes / (1024u * 1024u)) + " MB，请压缩后再粘贴");
        }
        // Same IHDR gate as the clipboard path: never let WIC allocate for a declared bomb frame.
        std::uint32_t ihdrWidth = 0;
        std::uint32_t ihdrHeight = 0;
        std::string limitError;
        const ImageStatus gate = CheckPngIhdr(static_cast<const std::uint8_t*>(pngBytes), size,
                                              ihdrWidth, ihdrHeight, limitError);
        if (gate != ImageStatus::Ok) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(gate, limitError);
        }
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CreateWicFactory(factory);
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法创建 WIC 工厂", hr));
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = OpenPngFrame(*factory.Get(), static_cast<const std::uint8_t*>(pngBytes), size, frame);
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("PNG 解码失败", hr));
        }
        UINT width = 0;
        UINT height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法读取 PNG 尺寸", hr));
        }
        if (width != ihdrWidth || height != ihdrHeight) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                "PNG 的 IHDR 尺寸与解码帧尺寸不一致");
        }
        std::string frameError;
        const ImageStatus frameLimits = CheckFrameLimits(width, height, frameError);
        if (frameLimits != ImageStatus::Ok) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(frameLimits, frameError);
        }
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法创建 WIC 格式转换器", hr));
        }
        // PNG is straight alpha; WIC performs the single premultiplication required by the swap chain.
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法转换为预乘 BGRA", hr));
        }
        // 96 DPI on purpose: using the PNG's own resolution would shrink high-DPI screenshots when
        // DrawBitmap maps DIPs to pixels. Callers decide the display size.
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> bitmap;
        hr = context.CreateBitmapFromWicBitmap(converter.Get(), &properties, bitmap.GetAddressOf());
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法创建 Direct2D 位图", hr));
        }
        Microsoft::WRL::ComPtr<ID2D1Bitmap> asBitmap;
        hr = bitmap.As(&asBitmap);
        if (FAILED(hr)) {
            return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed,
                                                                HresultDetail("无法获取 ID2D1Bitmap 接口", hr));
        }
        return Success<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(std::move(asBitmap));
    } catch (const std::bad_alloc&) {
        return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed, "内存不足，无法解码图片");
    } catch (const std::exception& exception) {
        return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(
            ImageStatus::DecodeFailed, std::string("解码图片发生异常：") + exception.what());
    } catch (...) {
        return Failure<Microsoft::WRL::ComPtr<ID2D1Bitmap>>(ImageStatus::DecodeFailed, "解码图片发生未知异常");
    }
}

const char* StatusText(ImageStatus status) noexcept {
    switch (status) {
        case ImageStatus::Ok: return "图片已就绪";
        case ImageStatus::TooLarge: {
            // Process-lifetime static: returning .c_str() of a temporary would dangle. The numbers
            // come from the product caps, so this fallback can never drift from the real gate.
            static const std::string message = [] {
                try {
                    return std::string("图片过大：单边不超过 ") + std::to_string(todo::kMaxImageDimension) +
                           " 像素、总像素不超过 " + std::to_string(todo::kMaxImagePixels) +
                           "、PNG 不超过 " + std::to_string(todo::kMaxImageBytes / (1024u * 1024u)) +
                           " MB，请压缩后再粘贴";
                } catch (...) {
                    return std::string("图片过大，请压缩后再粘贴");
                }
            }();
            return message.c_str();
        }
        case ImageStatus::DecodeFailed: return "无法读取剪贴板中的图片";
        case ImageStatus::Empty:
        default: return "剪贴板中没有图片";
    }
}

bool RunClipboardImageSelfTest(ID2D1DeviceContext* context, std::string& message) noexcept {
    try {
        // 3x2, 32bpp BI_RGB, bottom-up, alpha bytes deliberately zero. A correct decoder must
        // restore opacity and flip the rows back into top-down order.
        constexpr std::uint32_t kWidth = 3;
        constexpr std::uint32_t kHeight = 2;
        constexpr std::uint32_t kStride = 12;  // (3 * 32 + 31) / 32 * 4
        std::vector<std::uint8_t> dib(sizeof(BITMAPINFOHEADER) + kStride * kHeight, 0);
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = kWidth;
        header.biHeight = kHeight;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = kStride * kHeight;
        std::memcpy(dib.data(), &header, sizeof(header));
        std::uint8_t* bits = dib.data() + sizeof(BITMAPINFOHEADER);
        const auto put = [&](std::uint32_t bottomRow, std::uint32_t x, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
            std::uint8_t* pixel = bits + bottomRow * kStride + x * 4u;
            pixel[0] = b;
            pixel[1] = g;
            pixel[2] = r;
            pixel[3] = 0;
        };
        put(0, 0, 255, 0, 0);       // bottom-left: blue
        put(0, 1, 0, 255, 0);       // bottom-middle: green
        put(0, 2, 255, 255, 255);   // bottom-right: white
        put(1, 0, 0, 0, 255);       // top-left: red
        put(1, 1, 0, 0, 0);         // top-middle: black
        put(1, 2, 0, 255, 255);     // top-right: yellow

        const ClipboardImageResult encoded = EncodeDibBytes(dib, "CF_DIB");
        if (!encoded.ok()) {
            message = "DIB->PNG 失败：" + encoded.error;
            return false;
        }
        if (encoded.value.width != kWidth || encoded.value.height != kHeight) {
            message = "DIB->PNG 尺寸错误";
            return false;
        }
        static constexpr std::uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (encoded.value.bytes.size() < sizeof(kPngSignature) ||
            std::memcmp(encoded.value.bytes.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
            message = "编码结果不是 PNG";
            return false;
        }

        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CreateWicFactory(factory);
        if (FAILED(hr)) { message = HresultDetail("自测无法创建 WIC 工厂", hr); return false; }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> back;
        hr = DecodePngToBgra(*factory.Get(), encoded.value.bytes, width, height, back);
        if (FAILED(hr)) { message = HresultDetail("PNG 解码回 BGRA 失败", hr); return false; }
        const auto pixelAt = [&](std::uint32_t x, std::uint32_t y, std::uint32_t channel) -> std::uint8_t {
            return back[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
        };
        const bool pixelsOk = width == kWidth && height == kHeight &&
                              pixelAt(0, 0, 0) == 0 && pixelAt(0, 0, 1) == 0 && pixelAt(0, 0, 2) == 255 && pixelAt(0, 0, 3) == 255 &&
                              pixelAt(0, 1, 0) == 255 && pixelAt(0, 1, 1) == 0 && pixelAt(0, 1, 2) == 0 && pixelAt(0, 1, 3) == 255 &&
                              pixelAt(2, 0, 0) == 0 && pixelAt(2, 0, 1) == 255 && pixelAt(2, 0, 2) == 255 && pixelAt(2, 0, 3) == 255;
        if (!pixelsOk) { message = "PNG 解码后的像素或 alpha 不正确"; return false; }

        // Negative cases: truncated data and an oversized header must be distinguishable.
        const ClipboardImageResult truncated = EncodeDibBytes(std::vector<std::uint8_t>(dib.begin(), dib.begin() + 12), "CF_DIB");
        if (truncated.status != ImageStatus::DecodeFailed) { message = "截断的 DIB 没有被报告为解码失败"; return false; }
        std::vector<std::uint8_t> oversized = dib;
        BITMAPINFOHEADER wideHeader = header;
        wideHeader.biWidth = todo::kMaxImageDimension + 1;
        std::memcpy(oversized.data(), &wideHeader, sizeof(wideHeader));
        const ClipboardImageResult tooLarge = EncodeDibBytes(oversized, "CF_DIB");
        if (tooLarge.status != ImageStatus::TooLarge) { message = "超大 DIB 没有被报告为超限"; return false; }

        // IHDR decompression-bomb gate: a legal signature + IHDR declaring 30000x30000 with no
        // pixel data must be rejected as TooLarge BEFORE WIC ever sees it.
        std::vector<std::uint8_t> bomb(33, 0);
        std::memcpy(bomb.data(), kPngSignature, sizeof(kPngSignature));
        bomb[8] = 0; bomb[9] = 0; bomb[10] = 0; bomb[11] = 13;  // IHDR length = 13
        std::memcpy(bomb.data() + 12, "IHDR", 4);
        const std::uint32_t bombSide = 30000;
        bomb[16] = static_cast<std::uint8_t>(bombSide >> 24);
        bomb[17] = static_cast<std::uint8_t>(bombSide >> 16);
        bomb[18] = static_cast<std::uint8_t>(bombSide >> 8);
        bomb[19] = static_cast<std::uint8_t>(bombSide);
        bomb[20] = static_cast<std::uint8_t>(bombSide >> 24);
        bomb[21] = static_cast<std::uint8_t>(bombSide >> 16);
        bomb[22] = static_cast<std::uint8_t>(bombSide >> 8);
        bomb[23] = static_cast<std::uint8_t>(bombSide);
        const ClipboardImageResult bombResult = ValidatePngBytes(bomb);
        if (bombResult.status != ImageStatus::TooLarge) {
            message = "IHDR 解压炸弹没有被前置闸门拒绝";
            return false;
        }
        if (ValidatePngBytes(encoded.value.bytes).status != ImageStatus::Ok) {
            message = "IHDR 闸门误拒了正常 PNG";
            return false;
        }
        if (StripHtml(Utf8ToWide("A &amp; B&lt;C<br>&#65;")) != L"A & B<C\nA") {
            message = "CF_HTML 文本兜底（剥标签/解码实体）不正确";
            return false;
        }

        if (context) {
            const D2dBitmapResult bitmap = CreateBitmapFromPng(*context, encoded.value);
            if (!bitmap.ok()) { message = "CreateBitmapFromPng 失败：" + bitmap.error; return false; }
            const D2D1_SIZE_U size = bitmap.value->GetPixelSize();
            if (size.width != kWidth || size.height != kHeight) { message = "Direct2D 位图尺寸不正确"; return false; }
            message = "3x2 32bpp DIB -> PNG -> 32bppBGRA -> ID2D1Bitmap 全部通过";
        } else {
            message = "3x2 32bpp DIB -> PNG(" + std::to_string(encoded.value.bytes.size()) +
                      " 字节) -> 32bppBGRA 全部通过；alpha 强制不透明与行翻转已验证";
        }
        return true;
    } catch (const std::bad_alloc&) {
        message = "自测过程中内存不足";
        return false;
    } catch (...) {
        message = "自测过程中发生异常";
        return false;
    }
}

bool RunClipboardImageSelfTest(std::string& message) noexcept {
    return RunClipboardImageSelfTest(nullptr, message);
}

}  // namespace todo::ui
