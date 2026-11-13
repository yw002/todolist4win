#pragma once

#include <d2d1_1.h>

namespace todo::ui {

// The Lucide (ISC, v0.469.0) outline set the title bar uses. The identifier names the artwork, not
// the meaning: Sun and Moon are the same theme toggle in two states, and the caller picks which one
// to draw. See resources/icons/LICENSE for provenance.
enum class IconId { Pin, Database, Sun, Moon, RotateCw, Minus, Square, Close };

/**
 * Draws an icon so that its artwork fills `box` (square icons are centred, never stretched).
 *
 * Returns false when the render target does not expose ID2D1DeviceContext5 or the SVG document could
 * not be built, which lets a caller fall back to text instead of drawing nothing.
 *
 * `color` is baked into the artwork because Direct2D's SVG engine has no `currentColor`; the RGB
 * triple becomes part of the cache key, so a theme change simply builds a second document.
 */
[[nodiscard]] bool DrawIcon(ID2D1DeviceContext& context, IconId id, const D2D1_RECT_F& box, const D2D1_COLOR_F& color);

// Drops every cached document and stream. Call after a device-lost rebuild; DrawIcon also rebuilds on
// its own when it sees a different device context, so this is a belt-and-braces hook.
void ReleaseIconCache() noexcept;

}  // namespace todo::ui
