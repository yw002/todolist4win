#include "ui/HistoryPage.h"

#include "domain/TaskRules.h"
#include "domain/TimeRanges.h"
#include "ui/Dpi.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace todo::ui {

HistoryPage::HistoryPage(AppController& controller, std::function<void(const std::vector<std::int64_t>&)> deleteEvents) : controller_(controller) {
    search_.SetPlaceholder(L"搜索历史标题或备注");
    startDate_.SetPlaceholder(L"开始 YYYY-MM-DD");
    endDate_.SetPlaceholder(L"结束 YYYY-MM-DD");
    search_.SetOnChanged([this](const std::wstring& value) { controller_.SetHistoryKeyword(Dpi::WideToUtf8(value)); });
    search_.SetOnSubmit([this] { ApplyFilters(); });
    category_.SetText(L"全部分类");
    apply_.SetText(L"应用筛选");
    restore_.SetText(L"批量恢复");
    restore_.SetAccent(true);
    remove_.SetText(L"永久删除所选");
    remove_.SetDanger(true);
    apply_.SetOnClick([this] { ApplyFilters(); });
    category_.SetOnClick([this] {
        std::vector<PopupMenu::Item> items{{0, L"全部分类"}};
        for (const auto& item : controller_.State().categories) items.push_back({item.category.id, Dpi::Utf8ToWide(item.category.name)});
        categoryMenu_.Show(category_.Bounds().left, category_.Bounds().bottom + 4, 190, std::move(items), [this](std::int64_t id) { categoryId_ = id == 0 ? std::nullopt : std::optional<std::int64_t>(id); std::wstring name = L"全部分类"; for (const auto& item : controller_.State().categories) if (item.category.id == id) name = Dpi::Utf8ToWide(item.category.name); category_.SetText(name); });
    });
    restore_.SetOnClick([this] { controller_.RestoreBatch(list_.SelectedTaskIds()); });
    remove_.SetOnClick([this, deleteEvents] { const auto ids = list_.SelectedEventIds(); if (!ids.empty()) deleteEvents(ids); });
    list_.SetCallbacks([this](std::int64_t id) { controller_.Restore(id); }, [deleteEvents](std::int64_t id) { deleteEvents(std::vector<std::int64_t>{id}); }, [this] { controller_.LoadMore(); });
}

void HistoryPage::ApplyFilters() {
    auto& filter = controller_.State().historyFilter;
    controller_.SetHistoryKeyword(Dpi::WideToUtf8(search_.Text()));
    controller_.FlushPendingSearch(true);
    filter.categoryId = categoryId_;
    filter.timeRange.reset();
    const std::string start = TaskRules::Trim(Dpi::WideToUtf8(startDate_.Text()));
    const std::string end = TaskRules::Trim(Dpi::WideToUtf8(endDate_.Text()));
    if (!start.empty() || !end.empty()) {
        auto first = start.empty() ? TimeRanges::ParseDueDate("1970-01-01") : TimeRanges::ParseDueDate(start);
        auto last = end.empty() ? TimeRanges::ParseDueDate("9999-12-31") : TimeRanges::ParseDueDate(end);
        if (!first.ok() || !last.ok()) { controller_.ShowToast("历史日期格式无效"); return; }
        const LocalDateTime rangeEnd = end.empty() ? LocalDateTime{9999, 12, 31, 23, 59, 59} : TimeRanges::AddDays(TimeRanges::StartOfDay(last.value()), 1);
        filter.timeRange = TimeRange{TimeRanges::StartOfDay(first.value()), rangeEnd};
    }
    controller_.Navigate(Route::History);
}

void HistoryPage::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    const float left = bounds_.left + 16, right = bounds_.right - 16, width = right - left;
    resources.DrawText(L"历史事件", {left, bounds_.top + 8, left + 180, bounds_.top + 42}, nullptr, true);
    search_.SetBounds({left, bounds_.top + 52, left + width * 0.38F, bounds_.top + 90});
    startDate_.SetBounds({left + width * 0.38F + 8, bounds_.top + 52, left + width * 0.57F, bounds_.top + 90});
    endDate_.SetBounds({left + width * 0.57F + 8, bounds_.top + 52, left + width * 0.76F, bounds_.top + 90});
    category_.SetBounds({left + width * 0.76F + 8, bounds_.top + 52, right - 94, bounds_.top + 90});
    apply_.SetBounds({right - 86, bounds_.top + 52, right, bounds_.top + 90});
    restore_.SetBounds({right - 236, bounds_.top + 102, right - 114, bounds_.top + 140});
    remove_.SetBounds({right - 360, bounds_.top + 102, right - 244, bounds_.top + 140});
    list_.SetBounds({left, bounds_.top + 148, right, bounds_.bottom - 8});
    if (observedWrite_ != controller_.State().lastSuccessfulWrite) { observedWrite_ = controller_.State().lastSuccessfulWrite; list_.ClearSelection(); }
    const bool editable = !controller_.State().readOnly;
    restore_.SetEnabled(editable); remove_.SetEnabled(editable);
    list_.SetEvents(controller_.State().history);
    list_.SetReadOnly(!editable);
    list_.SetHasNextPage(controller_.State().hasHistoryNext);
    search_.Paint(context, resources); startDate_.Paint(context, resources); endDate_.Paint(context, resources);
    category_.Paint(context, resources); apply_.Paint(context, resources); restore_.Paint(context, resources); remove_.Paint(context, resources);
    list_.Paint(context, resources);
    categoryMenu_.Paint(context, resources);
}

bool HistoryPage::OnMouseDown(float x, float y, UINT flags) {
    if (categoryMenu_.IsOpen()) return true;
    Control* hit = nullptr;
    Control* const clickable[] = {&search_, &startDate_, &endDate_, &category_, &apply_, &restore_, &remove_, &list_};
    for (Control* control : clickable) if (control->HitTest(x, y)) { hit = control; break; }
    pressed_ = hit;
    if (focused_ && focused_ != hit) focused_->OnFocus(false);
    if (hit == &search_ || hit == &startDate_ || hit == &endDate_ || hit == &list_) { focused_ = hit; focused_->OnFocus(true); }
    return hit && hit->OnMouseDown(x, y, flags);
}
bool HistoryPage::OnMouseMove(float x, float y) {
    bool handled = false;
    Control* const hoverables[] = {&search_, &startDate_, &endDate_, &category_, &apply_, &restore_, &remove_, &list_};
    for (Control* control : hoverables) handled = control->OnMouseMove(x, y) || handled;
    categoryMenu_.OnMouseMove(x, y);
    return handled;
}
bool HistoryPage::OnMouseUp(float x, float y) { if (categoryMenu_.IsOpen()) return categoryMenu_.OnMouseUp(x, y); if (!pressed_) return false; auto* control = pressed_; pressed_ = nullptr; return control->OnMouseUp(x, y); }
bool HistoryPage::OnMouseWheel(float x, float y, int delta) { return list_.OnMouseWheel(x, y, delta); }
bool HistoryPage::OnKeyDown(UINT key, bool ctrl, bool shift) { return focused_ && focused_->OnKeyDown(key, ctrl, shift); }
void HistoryPage::FocusSearch() { if (focused_) focused_->OnFocus(false); focused_ = &search_; search_.OnFocus(true); }
TextBox* HistoryPage::ActiveTextBox() noexcept { return focused_ == &search_ ? &search_ : focused_ == &startDate_ ? &startDate_ : focused_ == &endDate_ ? &endDate_ : nullptr; }
Control* HistoryPage::FocusedControl() noexcept { return focused_; }
bool HistoryPage::HitTextField(float x, float y) const noexcept {
    if (!visible_) return false;
    for (const auto* field : {&search_, &startDate_, &endDate_}) {
        if (field->Visible() && field->HitTest(x, y)) return true;
    }
    return false;
}
bool HistoryPage::HitRow(float x, float y) const noexcept { return visible_ && list_.HitsRow(x, y); }

}  // namespace todo::ui
