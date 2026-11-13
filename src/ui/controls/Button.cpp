#include "ui/controls/Button.h"

#include <d2d1_1helper.h>
#include <algorithm>
#include <functional>
#include <wrl/client.h>

namespace todo::ui {

namespace {

// A secondary button that leans on colors_.panelStrong has no outline of its own in the light theme:
// panelStrong is white at 0.66 and the shared border brush is white at 0.85, which both disappear into
// the near-opaque popup surface. A muted tint keeps the surface neutral while giving the shape an
// edge. Only the light theme is affected - the dark palette already reads correctly.
constexpr float kSecondaryFillAlpha = 0.16F;
constexpr float kSecondaryEdgeAlpha = 0.38F;

}  // namespace

Button::Button() = default;
void Button::SetText(std::wstring text) { text_ = std::move(text); }
void Button::SetOnClick(std::function<void()> callback) { onClick_ = std::move(callback); }
void Button::SetAccent(bool accent) noexcept { accent_ = accent; }
void Button::SetDanger(bool danger) noexcept { danger_ = danger; }
void Button::SetRadius(float radius) noexcept { radius_ = radius; }
void Button::SetIcon(std::optional<IconId> id) noexcept { icon_ = id; }
void Button::SetIconSize(float dip) noexcept { iconSize_ = dip; }

void Button::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    const bool dark = resources.Colors().dark;
    // Hover only applies while the button is really interactive; a captured press keeps the
    // "pressed" look so the two states never fight over the transform.
    const bool hover = enabled_ && hovered_ && !pressed_;
    D2D1_MATRIX_3X2_F original;
    context.GetTransform(&original);
    const D2D1_POINT_2F center{(bounds_.left + bounds_.right) / 2, (bounds_.top + bounds_.bottom) / 2};
    if (pressed_) context.SetTransform(D2D1::Matrix3x2F::Scale(0.95F, 0.95F, center) * original);
    else if (hover) context.SetTransform(D2D1::Matrix3x2F::Scale(1.03F, 1.03F, center) * original);
    if (hover) {
        // Soft accent halo just outside the shape: this is what makes the hover state readable at
        // a glance on the near-black dark palette, where a brightness-only change is invisible.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glow;
        D2D1_COLOR_F glowColor = resources.Colors().accent;
        glowColor.a = 0.08F;
        context.CreateSolidColorBrush(glowColor, &glow);
        const D2D1_RECT_F halo{bounds_.left - 2, bounds_.top - 2, bounds_.right + 2, bounds_.bottom + 2};
        context.FillRoundedRectangle({{halo.left, halo.top, halo.right, halo.bottom}, radius_ + 1.5F, radius_ + 1.5F}, glow.Get());
    }
    // A secondary button is the only case that needs the tint; accent and danger buttons carry their
    // own colour and the palette border already sits on top of a saturated fill.
    const bool tintedSecondary = !accent_ && !danger_ && !dark;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    D2D1_COLOR_F color = accent_ ? resources.Colors().accent : danger_ ? resources.Colors().danger : resources.Colors().panelStrong;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondaryEdge;
    if (tintedSecondary) {
        color = resources.Colors().muted;
        color.a = kSecondaryFillAlpha;
        D2D1_COLOR_F edge = resources.Colors().muted;
        edge.a = kSecondaryEdgeAlpha;
        context.CreateSolidColorBrush(edge, &secondaryEdge);
    }
    if (hover) {
        // Interpolate in the direction that gains contrast: towards white on the dark theme,
        // towards black on the light one. 8% towards white was imperceptible before.
        const float mix = dark ? 0.16F : 0.07F;
        const float target = dark ? 1.0F : 0.0F;
        color.r += (target - color.r) * mix;
        color.g += (target - color.g) * mix;
        color.b += (target - color.b) * mix;
    }
    if (pressed_) { color.r *= 0.90F; color.g *= 0.90F; color.b *= 0.90F; }
    if (!enabled_) color.a *= 0.45F;
    context.CreateSolidColorBrush(color, &fill);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBorder;
    if (hover) {
        D2D1_COLOR_F borderColor = resources.Colors().accent;
        borderColor.a = 0.45F;
        context.CreateSolidColorBrush(borderColor, &hoverBorder);
    }
    ID2D1Brush* edge = resources.BorderBrush();
    if (hover) edge = hoverBorder.Get();
    else if (tintedSecondary) edge = secondaryEdge.Get();
    resources.DrawRoundRect(bounds_, radius_, fill.Get(), edge, hover ? 1.4F : 1.0F);
    D2D1_RECT_F textBounds = bounds_;
    textBounds.top += pressed_ ? 1.0F : 0.0F;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white;
    if (accent_ || danger_) context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);
    bool iconDrawn = false;
    if (icon_) {
        // The artwork takes exactly the colour the label would have used: white on a saturated accent
        // fill, the body text colour otherwise. An accent-coloured icon on an accent fill would be
        // invisible, so the text rule wins over the accent slot here.
        D2D1_COLOR_F iconColor = (accent_ || danger_) ? D2D1::ColorF(D2D1::ColorF::White) : resources.Colors().text;
        if (!enabled_) iconColor = resources.Colors().muted;
        const float box = std::min(iconSize_, std::min(bounds_.right - bounds_.left, bounds_.bottom - bounds_.top));
        const float centreY = (bounds_.top + bounds_.bottom) * 0.5F + (pressed_ ? 1.0F : 0.0F);
        const D2D1_RECT_F target{center.x - box * 0.5F, centreY - box * 0.5F, center.x + box * 0.5F, centreY + box * 0.5F};
        iconDrawn = DrawIcon(context, *icon_, target, iconColor);
    }
    // The label is only a fallback: it is drawn when no icon was requested, or when the render target
    // could not build the SVG document. That keeps a button from ever going blank.
    if (!iconDrawn) resources.DrawText(text_, textBounds, accent_ || danger_ ? white.Get() : resources.TextBrush(), false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    context.SetTransform(original);
}

bool Button::OnMouseDown(float x, float y, UINT) {
    if (!enabled_ || !HitTest(x, y)) return false;
    pressed_ = true;
    return true;
}
bool Button::OnMouseUp(float x, float y) {
    if (!pressed_) return false;
    pressed_ = false;
    if (enabled_ && HitTest(x, y) && onClick_) onClick_();
    return true;
}

}  // namespace todo::ui
