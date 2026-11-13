#include "ui/controls/PopupMenu.h"

#include <functional>
#include <vector>

namespace todo::ui {

void PopupMenu::Show(float x, float y, float width, std::vector<Item> items, std::function<void(std::int64_t)> callback) {
    items_ = std::move(items);
    onSelect_ = std::move(callback);
    bounds_ = {x, y, x + width, y + static_cast<float>(items_.size()) * 36.0F + 8.0F};
    visible_ = !items_.empty();
    hoveredIndex_ = -1;
}
void PopupMenu::Hide() noexcept { visible_ = false; items_.clear(); hoveredIndex_ = -1; }
bool PopupMenu::IsOpen() const noexcept { return visible_; }
void PopupMenu::Paint(ID2D1DeviceContext&, RenderResources& resources) {
    if (!visible_) return;
    resources.DrawGlassPanel(bounds_, 10.0F, true);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const D2D1_RECT_F row{bounds_.left + 4, bounds_.top + 4 + i * 36.0F, bounds_.right - 4, bounds_.top + 36 + i * 36.0F};
        if (static_cast<int>(i) == hoveredIndex_) resources.DrawRoundRect(row, 7, resources.PanelBrush());
        resources.DrawText(items_[i].text, {row.left + 8, row.top, row.right - 8, row.bottom}, items_[i].enabled ? resources.TextBrush() : resources.MutedBrush());
    }
}
bool PopupMenu::OnMouseMove(float x, float y) {
    if (!visible_) return false;
    hoveredIndex_ = HitTest(x, y) ? static_cast<int>((y - bounds_.top - 4) / 36.0F) : -1;
    return true;
}
bool PopupMenu::OnMouseUp(float x, float y) {
    if (!visible_) return false;
    const int index = HitTest(x, y) ? static_cast<int>((y - bounds_.top - 4) / 36.0F) : -1;
    if (index >= 0 && index < static_cast<int>(items_.size()) && items_[index].enabled) { const auto id = items_[index].id; auto callback = onSelect_; Hide(); if (callback) callback(id); }
    else Hide();
    return true;
}

}  // namespace todo::ui
