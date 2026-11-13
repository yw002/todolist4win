#include "ui/Backdrop.h"

#include <dwmapi.h>

namespace todo {
namespace {
// DWMWA_SYSTEMBACKDROP_TYPE is Windows 11 22H2+ and not declared by every SDK header we build with.
constexpr DWORD kDwmwaSystemBackdropType = 38;
// DWMSBT_TRANSIENTWINDOW (3) is Acrylic. Measured behaviour on this machine: the material blurs and
// desaturates whatever is behind the window, so a pure magenta sheet placed underneath produced a
// neutral ~(180,180,180) backdrop - that is real frosted glass, not a bug. It is the only way to get
// a blurred (rather than sharply readable) desktop behind the window, because DwmEnableBlurBehind-
// Window is a no-op since Windows 8 and a DirectComposition window has no redirection surface for
// DWM to blur into.
constexpr int kAcrylic = 3;
}  // namespace

bool Backdrop::Enable(HWND hwnd) noexcept {
    hwnd_ = hwnd;
    // Best effort only: the system backdrop is the single supported path. DwmEnableBlurBehindWindow
    // is not an option any more - since Windows 8 it returns S_OK without doing anything, and on a
    // frameless DirectComposition window it is known to combine into unresponsive input. Where the
    // attribute is unsupported (Windows 10) the call fails and the window simply stays opaque,
    // which is the intended degradation, so no fallback is attempted.
    int backdrop = kAcrylic;
    const HRESULT result = hwnd ? DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop)) : E_INVALIDARG;
    active_ = SUCCEEDED(result);
    return active_;
}

void Backdrop::Invalidate() noexcept {
    // The backdrop is a DWM window attribute rather than per-frame state, so re-applying it after a
    // DPI or size change is the only work required.
    if (hwnd_) Enable(hwnd_);
}

bool Backdrop::SystemBackdropActive() const noexcept { return active_; }

}  // namespace todo
