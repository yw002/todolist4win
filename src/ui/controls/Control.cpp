#include "ui/controls/Control.h"

namespace todo::ui {

void Control::Paint(ID2D1DeviceContext&, RenderResources&) {}
bool Control::HitTest(float x, float y) const noexcept { return visible_ && x >= bounds_.left && x < bounds_.right && y >= bounds_.top && y < bounds_.bottom; }
bool Control::OnMouseDown(float, float, UINT) { return false; }
bool Control::OnMouseUp(float, float) { return false; }
bool Control::OnMouseMove(float x, float y) { const bool old = hovered_; hovered_ = HitTest(x, y); return old != hovered_; }
bool Control::OnDoubleClick(float, float) { return false; }
bool Control::OnMouseWheel(float, float, int) { return false; }
bool Control::OnKeyDown(UINT, bool, bool) { return false; }
bool Control::OnChar(wchar_t) { return false; }
void Control::OnFocus(bool focused) { focused_ = focused; }
void Control::SetBounds(const D2D1_RECT_F& bounds) noexcept { bounds_ = bounds; }
void Control::SetVisible(bool visible) noexcept { visible_ = visible; }
void Control::SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
const D2D1_RECT_F& Control::Bounds() const noexcept { return bounds_; }
bool Control::Visible() const noexcept { return visible_; }
bool Control::Enabled() const noexcept { return enabled_; }
bool Control::Focused() const noexcept { return focused_; }

}  // namespace todo::ui
