#include "ui/Theme.h"

#include <windows.h>

#include <algorithm>

namespace todo {
namespace {
D2D1_COLOR_F Rgba(std::uint32_t rgb, float alpha = 1.0F) noexcept {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0F, static_cast<float>((rgb >> 8) & 0xFF) / 255.0F, static_cast<float>(rgb & 0xFF) / 255.0F, alpha};
}
D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    auto blend = [t](float x, float y) { return x + (y - x) * t; };
    return {blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b), blend(a.a, b.a)};
}
}

Theme::Theme() : current_(MakePalette(SystemUsesDarkTheme())), from_(current_), target_(current_) {}
void Theme::SetMode(ThemeMode mode) { mode_ = mode; target_ = MakePalette(mode == ThemeMode::Dark || (mode == ThemeMode::System && SystemUsesDarkTheme())); current_ = target_; from_ = target_; transitioning_ = false; }
void Theme::RefreshSystemTheme() { if (mode_ == ThemeMode::System) SetMode(ThemeMode::System); }
void Theme::BeginTransition(ThemeMode mode, double nowSeconds) {
    mode_ = mode;
    from_ = current_;
    target_ = MakePalette(mode == ThemeMode::Dark || (mode == ThemeMode::System && SystemUsesDarkTheme()));
    transitionStart_ = nowSeconds;
    transitioning_ = true;
}
bool Theme::Update(double nowSeconds) {
    if (!transitioning_) return false;
    const float t = std::clamp(static_cast<float>((nowSeconds - transitionStart_) / 0.300), 0.0F, 1.0F);
    current_.window = Mix(from_.window, target_.window, t);
    current_.panel = Mix(from_.panel, target_.panel, t);
    current_.panelStrong = Mix(from_.panelStrong, target_.panelStrong, t);
    current_.popup = Mix(from_.popup, target_.popup, t);
    current_.border = Mix(from_.border, target_.border, t);
    current_.text = Mix(from_.text, target_.text, t);
    current_.muted = Mix(from_.muted, target_.muted, t);
    current_.accent = Mix(from_.accent, target_.accent, t);
    current_.danger = Mix(from_.danger, target_.danger, t);
    current_.warning = Mix(from_.warning, target_.warning, t);
    current_.success = Mix(from_.success, target_.success, t);
    current_.dark = t < 0.5F ? from_.dark : target_.dark;
    if (t >= 1.0F) transitioning_ = false;
    return true;
}
const ThemePalette& Theme::Palette() const noexcept { return current_; }
ThemeMode Theme::Mode() const noexcept { return mode_; }

ThemePalette Theme::MakePalette(bool dark) noexcept {
    ThemePalette p;
    p.dark = dark;
    // Warm amber-on-glass identity: orange accent over a white/cream glass, both day and night.
    //
    // The alphas form a deliberate ladder, and the ladder is what makes the window read as glass:
    //   window      the whole client area              (lightest layer)
    //   panel       decorative strips: input row, footer
    //   panelStrong text fields, list rows, sidebar, popups - the surfaces text actually sits on
    // Because D2D composites each layer over the previous one, the effective opacity of a stack is
    // 1 - (1-a1)(1-a2)...; a panelStrong of 0.84 on top of a 0.58 window reaches 0.93 and the
    // desktop behind the window disappears completely. The values below keep the deepest stack near
    // 0.80 so the blurred desktop stays visible while text keeps its contrast.
    // The two themes cannot share an alpha. The acrylic material takes its brightness from the
    // *system* theme, not the app theme: with Windows in light mode the material behind the window is
    // a pale grey, so a dark body at 0.34 washes out to a milky grey and the dark theme stops being
    // dark. The dark surface therefore carries much more density than the light one.
    p.window = dark ? Rgba(0x1D150F, 0.70F) : Rgba(0xFFFAF5, 0.30F);
    p.panel = dark ? Rgba(0xFFFFFF, 0.08F) : Rgba(0xFFFFFF, 0.30F);
    p.panelStrong = dark ? Rgba(0x2B2018, 0.72F) : Rgba(0xFFFFFF, 0.66F);
    // A floating surface sits on top of everything else, so its alpha is not a style choice - below
    // roughly 0.92 the content underneath becomes legible through the popup and both layers lose.
    p.popup = dark ? Rgba(0x241A12, 0.97F) : Rgba(0xFFFDFB, 0.96F);
    p.border = dark ? Rgba(0xFFFFFF, 0.14F) : Rgba(0xFFFFFF, 0.85F);
    p.text = dark ? Rgba(0xF8F2EC) : Rgba(0x231A13);
    p.muted = dark ? Rgba(0xC2B2A2) : Rgba(0x8B7A6B);
    p.accent = dark ? Rgba(0xFFA45E) : Rgba(0xE8701F);
    p.danger = dark ? Rgba(0xFF7A6B) : Rgba(0xD2503F);
    p.warning = dark ? Rgba(0xFFC165) : Rgba(0xE39A2E);
    p.success = dark ? Rgba(0x62C892) : Rgba(0x3F9E6A);
    return p;
}

bool Theme::SystemUsesDarkTheme() noexcept {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

}  // namespace todo
