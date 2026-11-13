#include "ui/NavigationView.h"

#include <algorithm>
#include <functional>
#include <string>

#include "ui/Dpi.h"

namespace todo::ui {

NavigationView::NavigationView(AppController& controller) : controller_(controller) {}
void NavigationView::SetOnEditCategory(std::function<void(std::int64_t)> callback) { editCategory_ = std::move(callback); }
std::int64_t NavigationView::CategoryAt(float y) const {
    const auto& categories = controller_.State().categories;
    const int index = static_cast<int>((y - bounds_.top - 186) / 38.0F);
    const int maximumVisible = std::max(0, static_cast<int>((bounds_.bottom - 140 - (bounds_.top + 186)) / 38.0F));
    return index >= 0 && index < static_cast<int>(categories.size()) && index < maximumVisible ? categories[index].category.id : 0;
}

int NavigationView::RowAt(float y) const noexcept {
    // Mirrors the geometry used by Paint() and OnMouseUp(), so the highlight always sits exactly on
    // the row the click would activate.
    const float relative = y - bounds_.top;
    if (relative >= 12.0F && relative < 48.0F) return 0;
    if (relative >= 54.0F && relative < 90.0F) return 1;
    if (relative >= 96.0F && relative < 132.0F) return 2;
    const auto& categories = controller_.State().categories;
    const int index = static_cast<int>((y - bounds_.top - 186) / 38.0F);
    const int maximumVisible = std::max(0, static_cast<int>((bounds_.bottom - 140 - (bounds_.top + 186)) / 38.0F));
    if (index >= 0 && index < static_cast<int>(categories.size()) && index < maximumVisible && y >= bounds_.top + 186.0F + index * 38.0F &&
        y < bounds_.top + 186.0F + index * 38.0F + 34.0F)
        return kCategoryRowBase + index;
    if (y > bounds_.bottom - 54.0F && y < bounds_.bottom - 20.0F) return kHistoryRow;
    if (y > bounds_.bottom - 92.0F && y < bounds_.bottom - 58.0F) return kStatisticsRow;
    if (y > bounds_.bottom - 136.0F && y < bounds_.bottom - 96.0F) return kAddCategoryRow;
    return -1;
}

bool NavigationView::OnMouseMove(float x, float y) {
    Control::OnMouseMove(x, y);
    const int hover = HitTest(x, y) ? RowAt(y) : -1;
    if (hover == hoverIndex_) return false;
    hoverIndex_ = hover;
    return true;
}

bool NavigationView::ClearHover() noexcept {
    if (hoverIndex_ == -1) return false;
    hoverIndex_ = -1;
    return true;
}

int NavigationView::HoverIndex() const noexcept { return hoverIndex_; }

void NavigationView::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    resources.DrawGlassPanel(bounds_, 14, true);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
    D2D1_COLOR_F hoverColor = resources.Colors().accent;
    hoverColor.a = 0.18F;
    context.CreateSolidColorBrush(hoverColor, &hoverBrush);
    // The active row is a solid accent block, so its label must switch to white: the palette text
    // colour is near-black in light mode and would vanish on the orange fill.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> onAccent;
    context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &onAccent);
    const auto& state = controller_.State();
    struct Item { Route route; const wchar_t* text; std::int64_t count; };
    const Item items[] = {{Route::AllTasks, L"全部任务", state.stats.overview.pending + state.stats.overview.completed}, {Route::Today, L"今日待办", state.stats.overview.todayPending}, {Route::Overdue, L"逾期任务", state.stats.overview.overdue}};
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F row{bounds_.left + 10, bounds_.top + 12 + i * 42, bounds_.right - 10, bounds_.top + 48 + i * 42};
        const bool selected = state.route == items[i].route;
        // Hover is drawn first so the solid accent of the active route always wins.
        if (hoverIndex_ == i && !selected) resources.DrawRoundRect(row, 9, hoverBrush.Get());
        if (selected) resources.DrawRoundRect(row, 9, resources.AccentBrush());
        resources.DrawText(items[i].text, {row.left + 12, row.top, row.right - 42, row.bottom}, selected ? onAccent.Get() : nullptr);
        if (items[i].count > 0) resources.DrawText(std::to_wstring(items[i].count), {row.right - 40, row.top, row.right - 8, row.bottom}, selected ? onAccent.Get() : resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    resources.DrawText(L"分类", {bounds_.left + 14, bounds_.top + 148, bounds_.right - 14, bounds_.top + 174}, resources.MutedBrush(), false, true);
    const auto& categories = state.categories;
    const std::size_t maximumVisible = static_cast<std::size_t>(std::max(0, static_cast<int>((bounds_.bottom - 140 - (bounds_.top + 186)) / 38.0F)));
    for (std::size_t i = 0; i < categories.size() && i < maximumVisible; ++i) {
        const float y = bounds_.top + 186 + i * 38;
        const D2D1_RECT_F row{bounds_.left + 10, y, bounds_.right - 10, y + 34};
        const bool selected = state.route == Route::Category && state.categoryId == categories[i].category.id;
        if (!selected && hoverIndex_ == kCategoryRowBase + static_cast<int>(i)) resources.DrawRoundRect(row, 8, hoverBrush.Get());
        if (selected) resources.DrawRoundRect(row, 8, resources.AccentBrush());
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dot;
        const std::string color = categories[i].category.color;
        const unsigned rgb = static_cast<unsigned>(std::stoul(color.substr(1), nullptr, 16));
        context.CreateSolidColorBrush(D2D1::ColorF(rgb), &dot);
        context.FillEllipse({{row.left + 18, row.top + 17}, 4, 4}, dot.Get());
        resources.DrawText(Dpi::Utf8ToWide(categories[i].category.name), {row.left + 30, row.top, row.right - 38, row.bottom}, selected ? onAccent.Get() : nullptr);
        resources.DrawText(std::to_wstring(categories[i].pending), {row.right - 36, row.top, row.right - 8, row.bottom}, selected ? onAccent.Get() : resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    const float bottom = bounds_.bottom - 132;
    const D2D1_RECT_F addCategory{bounds_.left + 10, bounds_.bottom - 136, bounds_.right - 10, bounds_.bottom - 96};
    if (hoverIndex_ == kAddCategoryRow) resources.DrawRoundRect(addCategory, 8, hoverBrush.Get());
    resources.DrawText(L"+ 新增分类", {bounds_.left + 14, bottom, bounds_.right - 14, bottom + 30}, resources.AccentBrush());
    const D2D1_RECT_F history{bounds_.left + 10, bounds_.bottom - 54, bounds_.right - 10, bounds_.bottom - 20};
    const bool historySelected = state.route == Route::History;
    if (hoverIndex_ == kHistoryRow && !historySelected) resources.DrawRoundRect(history, 8, hoverBrush.Get());
    if (historySelected) resources.DrawRoundRect(history, 8, resources.AccentBrush());
    resources.DrawText(L"历史记录", {history.left + 12, history.top, history.right - 12, history.bottom}, historySelected ? onAccent.Get() : nullptr);
    const D2D1_RECT_F stats{bounds_.left + 10, bounds_.bottom - 92, bounds_.right - 10, bounds_.bottom - 58};
    const bool statisticsSelected = state.route == Route::Statistics;
    if (hoverIndex_ == kStatisticsRow && !statisticsSelected) resources.DrawRoundRect(stats, 8, hoverBrush.Get());
    if (statisticsSelected) resources.DrawRoundRect(stats, 8, resources.AccentBrush());
    resources.DrawText(L"统计分析", {stats.left + 12, stats.top, stats.right - 12, stats.bottom}, statisticsSelected ? onAccent.Get() : nullptr);
}

bool NavigationView::OnMouseUp(float x, float y) {
    if (!HitTest(x, y)) return false;
    const float relative = y - bounds_.top;
    if (relative < 54) controller_.Navigate(Route::AllTasks);
    else if (relative < 96) controller_.Navigate(Route::Today);
    else if (relative < 138) controller_.Navigate(Route::Overdue);
    else if (const auto id = CategoryAt(y)) controller_.Navigate(Route::Category, id);
    else if (y > bounds_.bottom - 54) controller_.Navigate(Route::History);
    else if (y > bounds_.bottom - 92) controller_.Navigate(Route::Statistics);
    else if (y > bounds_.bottom - 136 && y < bounds_.bottom - 96 && editCategory_) editCategory_(0);
    return true;
}
bool NavigationView::OnDoubleClick(float x, float y) { if (!HitTest(x, y)) return false; const auto id = CategoryAt(y); if (id && editCategory_) editCategory_(id); return true; }

}  // namespace todo::ui
