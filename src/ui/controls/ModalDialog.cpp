#include "ui/controls/ModalDialog.h"

#include <d2d1_1helper.h>
#include <functional>

namespace todo::ui {

void ModalDialog::Show(const D2D1_RECT_F& window, std::wstring title, std::wstring message, std::function<void(bool)> callback) {
    bounds_ = window;
    title_ = std::move(title);
    message_ = std::move(message);
    callback_ = std::move(callback);
    const float center = (window.left + window.right) / 2;
    const float top = (window.top + window.bottom) / 2 - 100;
    confirm_.SetBounds({center + 10, top + 146, center + 150, top + 184});
    cancel_.SetBounds({center - 150, top + 146, center - 10, top + 184});
    confirm_.SetText(L"确认");
    cancel_.SetText(L"取消");
    confirm_.SetAccent(true);
    confirm_.SetOnClick([this] { auto cb = callback_; Hide(); if (cb) cb(true); });
    cancel_.SetOnClick([this] { auto cb = callback_; Hide(); if (cb) cb(false); });
    visible_ = true;
}
void ModalDialog::Hide() noexcept { visible_ = false; callback_ = nullptr; }
bool ModalDialog::IsOpen() const noexcept { return visible_; }
bool ModalDialog::HitButton(float x, float y) const noexcept {
    if (!visible_) return false;
    return confirm_.HitTest(x, y) || cancel_.HitTest(x, y);
}
void ModalDialog::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    context.CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.28F), &dim);
    context.FillRectangle(bounds_, dim.Get());
    const D2D1_RECT_F card{confirm_.Bounds().left - 160, confirm_.Bounds().top - 146, confirm_.Bounds().right + 160, confirm_.Bounds().bottom + 18};
    resources.DrawGlassPanel(card, 14, true);
    resources.DrawText(title_, {card.left + 22, card.top + 18, card.right - 22, card.top + 48}, nullptr, true);
    resources.DrawText(message_, {card.left + 22, card.top + 60, card.right - 22, card.top + 112}, resources.MutedBrush());
    confirm_.Paint(context, resources);
    cancel_.Paint(context, resources);
}
bool ModalDialog::OnMouseDown(float x, float y, UINT flags) { if (!visible_) return false; if (confirm_.HitTest(x, y)) { pressedButton_ = true; return confirm_.OnMouseDown(x, y, flags); } if (cancel_.HitTest(x, y)) { pressedButton_ = false; return cancel_.OnMouseDown(x, y, flags); } return true; }
bool ModalDialog::OnMouseUp(float x, float y) { if (!visible_) return false; return pressedButton_ ? confirm_.OnMouseUp(x, y) : cancel_.OnMouseUp(x, y); }
bool ModalDialog::OnKeyDown(UINT key, bool, bool) { if (!visible_) return false; if (key == VK_ESCAPE) { auto cb = callback_; Hide(); if (cb) cb(false); } else if (key == VK_RETURN) { auto cb = callback_; Hide(); if (cb) cb(true); } return true; }

}  // namespace todo::ui
