#include "ui/controls/ModalDialog.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <functional>

namespace todo::ui {
namespace {

// 380x200 with a 24 DIP inset on every side. The old card grew out of the confirm button and ended
// up 460 DIP wide, left-aligned on that button, so the two buttons sat flush against its edges.
constexpr float kCardWidth = 380.0F;
constexpr float kCardHeight = 200.0F;
constexpr float kCardInset = 24.0F;
constexpr float kButtonWidth = 160.0F;
constexpr float kButtonHeight = 40.0F;

constexpr double kOpenSeconds = 0.22;
constexpr double kCloseSeconds = 0.15;

}  // namespace

void ModalDialog::Show(const D2D1_RECT_F& window, std::wstring title, std::wstring message, std::function<void(bool)> callback) {
    bounds_ = window;
    title_ = std::move(title);
    message_ = std::move(message);
    callback_ = std::move(callback);
    const float center = (window.left + window.right) / 2;
    const float top = (window.top + window.bottom) / 2 - kCardHeight * 0.5F;
    card_ = {center - kCardWidth * 0.5F, top, center + kCardWidth * 0.5F, top + kCardHeight};
    const float buttonTop = card_.bottom - kCardInset - kButtonHeight;
    cancel_.SetBounds({card_.left + kCardInset, buttonTop, card_.left + kCardInset + kButtonWidth, buttonTop + kButtonHeight});
    confirm_.SetBounds({card_.right - kCardInset - kButtonWidth, buttonTop, card_.right - kCardInset, buttonTop + kButtonHeight});
    confirm_.SetText(L"确认");
    cancel_.SetText(L"取消");
    confirm_.SetAccent(true);
    confirm_.SetOnClick([this] { auto cb = callback_; Hide(); if (cb) cb(true); });
    cancel_.SetOnClick([this] { auto cb = callback_; Hide(); if (cb) cb(false); });
    pressedButton_ = false;
    closing_ = false;
    shownAt_ = animationClock_.Now();
    visible_ = true;
    if (requestFrame_) requestFrame_();
}

void ModalDialog::Hide() noexcept {
    if (visible_ && AnimationClock::AnimationsEnabled()) {
        closing_ = true;
        closingAt_ = animationClock_.Now();
        if (requestFrame_) requestFrame_();
        return;
    }
    FinishHide();
}

void ModalDialog::FinishHide() noexcept {
    visible_ = false;
    closing_ = false;
    pressedButton_ = false;
    callback_ = nullptr;
}

bool ModalDialog::Tick(double now) noexcept {
    if (closing_) {
        if (now - closingAt_ >= kCloseSeconds) {
            FinishHide();
            return false;
        }
        return true;
    }
    return visible_ && AnimationClock::AnimationsEnabled() && now - shownAt_ < kOpenSeconds;
}

bool ModalDialog::IsOpen() const noexcept { return visible_; }

bool ModalDialog::HitButton(float x, float y) const noexcept {
    if (!visible_) return false;
    return confirm_.HitTest(x, y) || cancel_.HitTest(x, y);
}

void ModalDialog::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    const double now = animationClock_.Now();
    float progress = 1.0F;
    if (AnimationClock::AnimationsEnabled()) {
        progress = closing_
                       ? 1.0F - static_cast<float>((now - closingAt_) / kCloseSeconds)
                       : static_cast<float>((now - shownAt_) / kOpenSeconds);
        progress = std::clamp(progress, 0.0F, 1.0F);
    }
    const float closed = closing_ ? 1.0F - progress : 0.0F;
    const float scale = closing_ ? 1.0F - 0.035F * closed : 0.94F + 0.06F * AnimationClock::EaseOutBack(progress);
    const float fade = closing_ ? progress : std::min(1.0F, progress * 2.0F);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    context.CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.28F * fade), &dim);
    context.FillRectangle(bounds_, dim.Get());

    D2D1_MATRIX_3X2_F savedTransform{};
    context.GetTransform(&savedTransform);
    if (scale != 1.0F) {
        context.SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
            D2D1::Point2F((card_.left + card_.right) * 0.5F, (card_.top + card_.bottom) * 0.5F)));
    }
    resources.DrawGlassPanel(card_, 14, true);
    resources.DrawText(title_, {card_.left + kCardInset, card_.top + 20, card_.right - kCardInset, card_.top + 54}, nullptr, true);
    resources.DrawText(message_, {card_.left + kCardInset, card_.top + 64, card_.right - kCardInset, card_.top + 120}, resources.MutedBrush());
    confirm_.Paint(context, resources);
    cancel_.Paint(context, resources);
    context.SetTransform(savedTransform);
}

bool ModalDialog::OnMouseDown(float x, float y, UINT flags) {
    if (!visible_ || closing_) return true;
    if (confirm_.HitTest(x, y)) { pressedButton_ = true; return confirm_.OnMouseDown(x, y, flags); }
    if (cancel_.HitTest(x, y)) { pressedButton_ = false; return cancel_.OnMouseDown(x, y, flags); }
    return true;
}
bool ModalDialog::OnMouseUp(float x, float y) {
    if (!visible_ || closing_) return true;
    return pressedButton_ ? confirm_.OnMouseUp(x, y) : cancel_.OnMouseUp(x, y);
}
bool ModalDialog::OnKeyDown(UINT key, bool, bool) {
    if (!visible_ || closing_) return true;
    if (key == VK_ESCAPE) { auto cb = callback_; Hide(); if (cb) cb(false); }
    else if (key == VK_RETURN) { auto cb = callback_; Hide(); if (cb) cb(true); }
    return true;
}

}  // namespace todo::ui
