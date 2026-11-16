#include "ui/IconRenderer.h"

#include <d2d1_1helper.h>
#include <d2d1_3.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace todo::ui {
namespace {

// The SVGs are embedded verbatim instead of read from resources/icons at run time: the installed
// application has no resources directory next to the executable, and a missing file would leave an
// empty button. resources/icons/ keeps the files as the provenance record and the ISC licence.
constexpr const char* kPinSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-pin"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <path d="M12 17v5" />
  <path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z" />
</svg>)svg";

constexpr const char* kDatabaseSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-database"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <ellipse cx="12" cy="5" rx="9" ry="3" />
  <path d="M3 5V19A9 3 0 0 0 21 19V5" />
  <path d="M3 12A9 3 0 0 0 21 12" />
</svg>)svg";

constexpr const char* kSunSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-sun"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <circle cx="12" cy="12" r="4" />
  <path d="M12 2v2" />
  <path d="M12 20v2" />
  <path d="m4.93 4.93 1.41 1.41" />
  <path d="m17.66 17.66 1.41 1.41" />
  <path d="M2 12h2" />
  <path d="M20 12h2" />
  <path d="m6.34 17.66-1.41 1.41" />
  <path d="m19.07 4.93-1.41 1.41" />
</svg>)svg";

constexpr const char* kMoonSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-moon"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z" />
</svg>)svg";

constexpr const char* kRotateCwSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-rotate-cw"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8" />
  <path d="M21 3v5h-5" />
</svg>)svg";

constexpr const char* kMinusSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-minus"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <path d="M5 12h14" />
</svg>)svg";

constexpr const char* kSquareSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-square"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <rect width="18" height="18" x="3" y="3" rx="2" />
</svg>)svg";

constexpr const char* kRestoreSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-copy"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <rect width="14" height="14" x="8" y="8" rx="2" ry="2" />
  <path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2" />
</svg>)svg";

constexpr const char* kCloseSvg = R"svg(<!-- @license lucide-static v0.469.0 - ISC -->
<svg
  class="lucide lucide-x"
  xmlns="http://www.w3.org/2000/svg"
  width="24"
  height="24"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
>
  <path d="M18 6 6 18" />
  <path d="m6 6 12 12" />
</svg>)svg";

struct IconSource {
    IconId id;
    const char* svg;
};

constexpr std::array<IconSource, 9> kSources{{
    {IconId::Pin, kPinSvg},
    {IconId::Database, kDatabaseSvg},
    {IconId::Sun, kSunSvg},
    {IconId::Moon, kMoonSvg},
    {IconId::RotateCw, kRotateCwSvg},
    {IconId::Minus, kMinusSvg},
    {IconId::Square, kSquareSvg},
    {IconId::Restore, kRestoreSvg},
    {IconId::Close, kCloseSvg},
}};

// Every Lucide icon is 24x24. Keeping the document at its natural size and scaling with the device
// transform means one cached document serves every on-screen size.
constexpr float kViewportSize = 24.0F;

constexpr const char* kCurrentColorToken = "currentColor";

struct CacheEntry {
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> document;  // null when the source failed to parse
    ID2D1DeviceContext5* owner{nullptr};
};

// Single-threaded UI: the renderer runs on the window thread, so no locking is needed. Keyed by
// (artwork, colour) because the stroke colour is baked into the document text.
std::map<std::pair<int, unsigned>, CacheEntry>& Cache() {
    static std::map<std::pair<int, unsigned>, CacheEntry> cache;
    return cache;
}

[[nodiscard]] const char* SourceOf(IconId id) noexcept {
    for (const auto& entry : kSources) {
        if (entry.id == id) return entry.svg;
    }
    return kPinSvg;
}

[[nodiscard]] unsigned PackColor(const D2D1_COLOR_F& color) noexcept {
    auto channel = [](float value) {
        return static_cast<unsigned>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
}

// Direct2D's SVG engine does not understand the CSS keyword `currentColor`; leaving it in place makes
// the stroke resolve to black. Substituting a literal hex keeps the rest of the artwork untouched, so
// the shapes keep the rounded joins and caps that make the set look coherent.
[[nodiscard]] std::string PaintSource(const char* svg, unsigned rgb) {
    char hex[8] = {};
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", (rgb >> 16) & 0xFFU, (rgb >> 8) & 0xFFU, rgb & 0xFFU);
    std::string text(svg);
    const std::size_t tokenLength = std::strlen(kCurrentColorToken);
    for (std::size_t at = text.find(kCurrentColorToken); at != std::string::npos; at = text.find(kCurrentColorToken, at + 7)) {
        text.replace(at, tokenLength, hex);
    }
    return text;
}

[[nodiscard]] ID2D1SvgDocument* Document(ID2D1DeviceContext5* context, IconId id, const D2D1_COLOR_F& color) {
    const unsigned rgb = PackColor(color);
    const std::pair<int, unsigned> key{static_cast<int>(id), rgb};
    auto& cache = Cache();
    const auto found = cache.find(key);
    // A cached document was created by one device context. A device-lost rebuild produces a new one,
    // so a mismatched owner rebuilds instead of handing a stale resource to a fresh target.
    if (found != cache.end() && found->second.owner == context) return found->second.document.Get();
    CacheEntry entry;
    entry.owner = context;
    const std::string painted = PaintSource(SourceOf(id), rgb);
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(painted.data()), static_cast<UINT>(painted.size())));
    if (stream && SUCCEEDED(context->CreateSvgDocument(stream.Get(), D2D1::SizeF(kViewportSize, kViewportSize), &entry.document))) {
        // Created. A failure leaves the document null and the caller falls back to text.
    }
    cache[key] = entry;
    return entry.document.Get();
}

}  // namespace

bool DrawIcon(ID2D1DeviceContext& context, IconId id, const D2D1_RECT_F& box, const D2D1_COLOR_F& color) {
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> context5;
    if (FAILED(context.QueryInterface(IID_PPV_ARGS(&context5))) || !context5) return false;
    const float side = std::min(box.right - box.left, box.bottom - box.top);
    if (!(side > 0.0F)) return false;
    ID2D1SvgDocument* document = Document(context5.Get(), id, color);
    if (document == nullptr) return false;
    // Compose to the right of the current transform: the caller's matrix already maps DIP to pixels,
    // and this one maps the 24 DIP viewport onto the centred square inside `box`.
    const float scale = side / kViewportSize;
    const float left = (box.left + box.right) * 0.5F - side * 0.5F;
    const float top = (box.top + box.bottom) * 0.5F - side * 0.5F;
    D2D1_MATRIX_3X2_F previous;
    context5->GetTransform(&previous);
    context5->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(left, top) * previous);
    context5->DrawSvgDocument(document);
    context5->SetTransform(previous);
    return true;
}

void ReleaseIconCache() noexcept { Cache().clear(); }

}  // namespace todo::ui
