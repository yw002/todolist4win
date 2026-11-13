#include "ui/controls/HistoryListControl.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/Dpi.h"

namespace todo::ui {

HistoryListControl::HistoryListControl() { list_.SetRowHeight(86); list_.SetPageLoader([this](int page) { if (page > 0 && hasNext_ && loadMore_) loadMore_(); }); }
void HistoryListControl::SetEvents(const std::vector<HistoryEvent>& events) { if (events_ != &events) list_.ClearPages(); events_ = &events; list_.SetItemCount(static_cast<int>(events.size())); list_.MarkPageLoaded(0); }
void HistoryListControl::SetReadOnly(bool value) noexcept { readOnly_ = value; }
void HistoryListControl::SetHasNextPage(bool value) noexcept { hasNext_ = value; }
void HistoryListControl::SetCallbacks(std::function<void(std::int64_t)> restore, std::function<void(std::int64_t)> remove, std::function<void()> loadMore) { restore_ = std::move(restore); remove_ = std::move(remove); loadMore_ = std::move(loadMore); }
std::vector<std::int64_t> HistoryListControl::SelectedEventIds() const { return {selected_.begin(), selected_.end()}; }
std::vector<std::int64_t> HistoryListControl::SelectedTaskIds() const {
    std::vector<std::int64_t> result;
    if (!events_) return result;
    for (const auto& event : *events_) if (selected_.contains(event.id)) result.push_back(event.taskId);
    return result;
}
void HistoryListControl::ClearSelection() { selected_.clear(); }
std::optional<int> HistoryListControl::RowAt(float y) const { return list_.RowIndexAt(y); }
bool HistoryListControl::HitsRow(float x, float y) const noexcept {
    if (!visible_ || !events_ || events_->empty() || !HitTest(x, y)) return false;
    const std::optional<int> row = RowAt(y);
    return row.has_value() && *row >= 0 && *row < static_cast<int>(events_->size());
}

void HistoryListControl::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    list_.SetViewport(bounds_);
    list_.EnsureVisibleRange();
    context.PushAxisAlignedClip(bounds_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!events_ || events_->empty()) {
        resources.DrawText(L"暂无历史事件", {bounds_.left, bounds_.top + 70, bounds_.right, bounds_.top + 106}, nullptr, true, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        resources.DrawText(L"完成任务后，完成与取消完成事件会显示在这里", {bounds_.left, bounds_.top + 116, bounds_.right, bounds_.top + 144}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        context.PopAxisAlignedClip();
        return;
    }
    const auto [first, last] = list_.VisibleRange();
    for (int i = first; i < last && i < static_cast<int>(events_->size()); ++i) {
        const auto& event = (*events_)[i];
        const float y = bounds_.top + i * 86.0F - list_.Offset();
        const D2D1_RECT_F row{bounds_.left + 4, y + 4, bounds_.right - 4, y + 82};
        resources.DrawRoundRect(row, 12, resources.PanelBrush(), resources.BorderBrush());
        const bool selected = selected_.contains(event.id);
        const D2D1_RECT_F check{row.left + 12, row.top + 27, row.left + 30, row.top + 45};
        resources.DrawRoundRect(check, 4, selected ? resources.AccentBrush() : nullptr, selected ? resources.AccentBrush() : resources.MutedBrush());
        if (selected) resources.DrawText(L"✓", check, resources.TextBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        resources.DrawText(Dpi::Utf8ToWide(event.snapshot.title), {row.left + 44, row.top + 8, row.right - 148, row.top + 36});
        const std::string operation = event.operationType == OperationType::Complete ? "完成" : "取消完成";
        const std::string category = event.snapshot.originalCategoryName.empty() ? "未分类" : event.snapshot.originalCategoryName;
        const std::string detail = operation + "  ·  " + category + "  ·  事件 " + event.eventAt.ToString();
        resources.DrawText(Dpi::Utf8ToWide(detail), {row.left + 44, row.top + 38, row.right - 148, row.bottom - 6}, resources.MutedBrush(), false, true);
        if (event.operationType == OperationType::Complete && !readOnly_) resources.DrawText(L"恢复", {row.right - 134, row.top + 16, row.right - 78, row.top + 42}, resources.AccentBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hoverRow_ == i && !readOnly_) resources.DrawText(L"永久删除", {row.right - 76, row.top + 16, row.right - 10, row.top + 42}, resources.DangerBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (list_.MaximumOffset() > 0) {
        const float track = bounds_.bottom - bounds_.top;
        const float thumb = std::max(28.0F, track * track / list_.ContentHeight());
        const float y = bounds_.top + (track - thumb) * list_.Offset() / list_.MaximumOffset();
        resources.DrawRoundRect({bounds_.right - 5, y, bounds_.right - 1, y + thumb}, 2, resources.MutedBrush());
    }
    context.PopAxisAlignedClip();
}

bool HistoryListControl::OnMouseDown(float x, float y, UINT) {
    if (!HitTest(x, y) || !events_) return false;
    auto row = RowAt(y);
    if (!row) return false;
    const auto& event = (*events_)[*row];
    pressedId_ = event.id;
    const float relative = x - bounds_.left;
    pressedAction_ = relative < 40 ? 1 : (x > bounds_.right - 144 && x < bounds_.right - 76 ? 2 : x >= bounds_.right - 76 ? 3 : 0);
    return true;
}
bool HistoryListControl::OnMouseUp(float x, float y) {
    if (!pressedId_ || !events_) return false;
    auto row = RowAt(y);
    if (row && *row < static_cast<int>(events_->size()) && (*events_)[*row].id == pressedId_) {
        const auto& event = (*events_)[*row];
        if (pressedAction_ == 1) { if (selected_.contains(event.id)) selected_.erase(event.id); else selected_.insert(event.id); }
        else if (!readOnly_ && pressedAction_ == 2 && event.operationType == OperationType::Complete && restore_) restore_(event.taskId);
        else if (!readOnly_ && pressedAction_ == 3 && remove_) remove_(event.id);
    }
    pressedId_ = 0;
    pressedAction_ = 0;
    return true;
}
bool HistoryListControl::OnMouseMove(float x, float y) { Control::OnMouseMove(x, y); hoverRow_ = RowAt(y).value_or(-1); return HitTest(x, y); }
bool HistoryListControl::OnMouseWheel(float x, float y, int delta) { if (!HitTest(x, y)) return false; list_.ScrollByWheel(delta); hoverRow_ = RowAt(y).value_or(-1); if (hasNext_ && list_.Offset() >= list_.MaximumOffset() - 86 && loadMore_) loadMore_(); return true; }

}  // namespace todo::ui
