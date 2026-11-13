#pragma once

#include <windows.h>

#include <d2d1_1.h>

namespace todo::CursorFactory {

// Builds (once per DPI and accent colour) and returns the application arrow cursor: a translucent
// warm-white pointer with a 1px contour and a soft three-ring shadow. The result is cached because
// CreateIconIndirect allocates GDI/USER objects, and rebuilding it on every WM_SETCURSOR would
// exhaust the handle quota within seconds. Hotspot is the arrow tip, so the click point stays where
// the user aims.
//
// dpi     - window DPI; the bitmap is drawn at 32 * dpi / 96 pixels so it stays crisp.
// accent  - theme accent; only used to warm the glass body, never as an ornament on the shape.
//
// Returns nullptr only when the icon cannot be created; callers fall back to IDC_ARROW.
[[nodiscard]] HCURSOR Arrow(UINT dpi, D2D1_COLOR_F accent) noexcept;

// Releases every cached HCURSOR. Must be called before the process drops its message loop
// (MainWindow calls it from WM_DESTROY); the cache rebuilds itself on demand afterwards.
void Destroy() noexcept;

}  // namespace todo::CursorFactory
