#include "ui/controls/ScrollBar.h"

#include <algorithm>
#include <functional>

namespace todo::ui {

void ScrollBar::SetRange(int maximum, int viewport) noexcept { maximum_ = std::max(0, maximum); viewport_ = std::max(0, viewport); offset_ = std::clamp(offset_, 0, std::max(0, maximum_ - viewport_)); }
void ScrollBar::SetOffset(int offset) noexcept { offset_ = std::clamp(offset, 0, std::max(0, maximum_ - viewport_)); }
void ScrollBar::SetOnScroll(std::function<void(int)> callback) { onScroll_ = std::move(callback); }
void ScrollBar::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_ || maximum_ <= viewport_ || bounds_.bottom <= bounds_.top) return;
    const float height = bounds_.bottom - bounds_.top;
    const float thumbHeight = std::max(24.0F, height * static_cast<float>(viewport_) / maximum_);
    const float travel = height - thumbHeight;
    const float y = bounds_.top + travel * static_cast<float>(offset_) / std::max(1, maximum_ - viewport_);
    resources.DrawRoundRect({bounds_.left, y, bounds_.right, y + thumbHeight}, (bounds_.right - bounds_.left) / 2, resources.MutedBrush());
}
bool ScrollBar::OnMouseDown(float x, float y, UINT) { if (!HitTest(x, y)) return false; dragging_ = true; ScrollTo(y); return true; }
bool ScrollBar::OnMouseMove(float x, float y) { Control::OnMouseMove(x, y); if (dragging_) ScrollTo(y); return dragging_; }
bool ScrollBar::OnMouseUp(float, float) { if (!dragging_) return false; dragging_ = false; return true; }
void ScrollBar::ScrollTo(float y) {
    const float height = bounds_.bottom - bounds_.top;
    const float thumbHeight = std::max(24.0F, height * static_cast<float>(viewport_) / std::max(1, maximum_));
    const float ratio = std::clamp((y - bounds_.top - thumbHeight / 2) / std::max(1.0F, height - thumbHeight), 0.0F, 1.0F);
    const int value = static_cast<int>(ratio * std::max(0, maximum_ - viewport_) + 0.5F);
    if (value != offset_) { offset_ = value; if (onScroll_) onScroll_(value); }
}

}  // namespace todo::ui
