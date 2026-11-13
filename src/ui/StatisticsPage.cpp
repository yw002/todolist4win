#include "ui/StatisticsPage.h"

namespace todo::ui {

StatisticsPage::StatisticsPage(AppController& controller) : controller_(controller) { refresh_.SetText(L"刷新统计"); refresh_.SetOnClick([this] { controller_.Refresh(); }); }
void StatisticsPage::SetTime(double nowSeconds) noexcept { nowSeconds_ = nowSeconds; }
void StatisticsPage::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    resources.DrawText(L"统计分析", {bounds_.left + 16, bounds_.top + 8, bounds_.left + 180, bounds_.top + 44}, nullptr, true);
    refresh_.SetBounds({bounds_.right - 126, bounds_.top + 10, bounds_.right - 16, bounds_.top + 46});
    charts_.SetBounds({bounds_.left + 16, bounds_.top + 58, bounds_.right - 16, bounds_.bottom - 12});
    if (statsVersion_ != controller_.State().dataVersion || statsVersion_ == static_cast<std::uint64_t>(-1)) { statsVersion_ = controller_.State().dataVersion; charts_.SetStats(controller_.State().stats, nowSeconds_); }
    charts_.SetTime(nowSeconds_);
    charts_.Paint(context, resources);
    refresh_.Paint(context, resources);
}
bool StatisticsPage::OnMouseDown(float x, float y, UINT flags) { return refresh_.OnMouseDown(x, y, flags); }
bool StatisticsPage::OnMouseUp(float x, float y) { return refresh_.OnMouseUp(x, y); }

}  // namespace todo::ui
