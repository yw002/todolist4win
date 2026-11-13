#include "ui/ToastView.h"

namespace todo::ui {
void ToastView::Show(std::wstring text, double nowSeconds) { text_ = std::move(text); shownAt_ = nowSeconds; visible_ = !text_.empty(); }
void ToastView::SetTime(double nowSeconds) noexcept { nowSeconds_ = nowSeconds; if (nowSeconds_ - shownAt_ > 4.0) visible_ = false; }
void ToastView::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_ || text_.empty()) return;
    const float width = bounds_.right - bounds_.left;
    // Lifted above the status strip: the persistent connection error now lives in that strip's
    // right-hand side, and the two would otherwise draw over each other.
    const D2D1_RECT_F toast{bounds_.left + width * 0.25F, bounds_.bottom - 122, bounds_.right - width * 0.25F, bounds_.bottom - 78};
    resources.DrawGlassPanel(toast, 12, true);
    resources.DrawText(text_, {toast.left + 18, toast.top, toast.right - 18, toast.bottom}, nullptr, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    Control::Paint(context, resources);
}
}
