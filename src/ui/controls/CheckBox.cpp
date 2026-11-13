#include "ui/controls/CheckBox.h"

#include <algorithm>

#include <d2d1_1helper.h>
#include <functional>
#include <wrl/client.h>

namespace todo::ui {

void CheckBox::SetChecked(bool checked, double nowSeconds) noexcept { if (checked_ != checked) animationStart_ = nowSeconds; checked_ = checked; }
void CheckBox::SetOnToggle(std::function<void(bool)> callback) { onToggle_ = std::move(callback); }
bool CheckBox::Checked() const noexcept { return checked_; }

void CheckBox::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    const float size = std::min(bounds_.right - bounds_.left, bounds_.bottom - bounds_.top);
    D2D1_ELLIPSE circle{{(bounds_.left + bounds_.right) / 2, (bounds_.top + bounds_.bottom) / 2}, size / 2, size / 2};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    context.CreateSolidColorBrush(checked_ ? resources.Colors().accent : resources.Colors().panelStrong, &fill);
    context.FillEllipse(circle, fill.Get());
    context.DrawEllipse(circle, checked_ ? resources.AccentBrush() : resources.MutedBrush(), 1.2F);
    if (checked_) {
        D2D1_POINT_2F a{circle.point.x - size * 0.22F, circle.point.y};
        D2D1_POINT_2F b{circle.point.x - size * 0.05F, circle.point.y + size * 0.18F};
        D2D1_POINT_2F c{circle.point.x + size * 0.26F, circle.point.y - size * 0.20F};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white;
        context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);
        context.DrawLine(a, b, white.Get(), 2.0F);
        context.DrawLine(b, c, white.Get(), 2.0F);
    }
}

bool CheckBox::OnMouseUp(float x, float y) {
    if (!enabled_ || !HitTest(x, y)) return false;
    if (onToggle_) onToggle_(!checked_);
    return true;
}

}  // namespace todo::ui
