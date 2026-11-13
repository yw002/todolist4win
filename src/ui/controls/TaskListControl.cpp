#include "ui/controls/TaskListControl.h"

#include <algorithm>
#include <cmath>

#include <d2d1_1helper.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "domain/TimeRanges.h"
#include "ui/Dpi.h"
#include "ui/AnimationClock.h"

namespace todo::ui {

TaskListControl::TaskListControl() { list_.SetRowHeight(74); list_.SetPageLoader([this](int page) { if (page > 0 && hasNext_ && loadMore_) loadMore_(); }); }
void TaskListControl::SetTasks(const std::vector<Task>& tasks) {
    if (tasks_ != &tasks) { list_.ClearPages(); knownTasks_.clear(); removedRows_.clear(); }
    else {
        std::unordered_set<std::int64_t> nextIds;
        for (const auto& task : tasks) nextIds.insert(task.id);
        if (nextIds != knownTasks_) {
            removedRows_.clear();
            for (std::size_t i = 0; i < tasks_->size(); ++i) if (!nextIds.contains((*tasks_)[i].id)) removedRows_.push_back({(*tasks_)[i], static_cast<int>(i)});
            if (!removedRows_.empty()) removedAt_ = nowSeconds_;
        }
    }
    tasks_ = &tasks;
    std::unordered_set<std::int64_t> current;
    for (const auto& task : tasks) {
        current.insert(task.id);
        if (!knownTasks_.contains(task.id)) enterAnimations_[task.id] = nowSeconds_;
        if (!completionAnimations_.contains(task.id)) completionAnimations_[task.id] = nowSeconds_ - 1.0;
        if (completionStates_.contains(task.id) && completionStates_[task.id] != task.completed) completionAnimations_[task.id] = nowSeconds_;
        completionStates_[task.id] = task.completed;
    }
    knownTasks_ = std::move(current);
    list_.SetItemCount(static_cast<int>(tasks.size()));
    list_.MarkPageLoaded(0);
    if (!tasks.empty() && selectedId_ == 0) selectedId_ = tasks.front().id;
}
void TaskListControl::SetCategories(const std::vector<CategoryCount>& categories) { categories_ = categories; }
void TaskListControl::SetReadOnly(bool readOnly) noexcept { readOnly_ = readOnly; }
void TaskListControl::SetCanReorder(bool canReorder) noexcept { canReorder_ = canReorder; }
void TaskListControl::SetHasNextPage(bool value) noexcept { hasNext_ = value; }
void TaskListControl::SetCallbacks(std::function<void(const Task&, bool)> toggle, std::function<void(const Task&)> edit, std::function<void(const Task&)> remove, std::function<void(std::int64_t, std::optional<std::int64_t>, std::optional<std::int64_t>)> reorder, std::function<void()> loadMore) {
    toggle_ = std::move(toggle); edit_ = std::move(edit); remove_ = std::move(remove); reorder_ = std::move(reorder); loadMore_ = std::move(loadMore);
}
void TaskListControl::SetTime(double nowSeconds) noexcept { nowSeconds_ = nowSeconds; }
void TaskListControl::Clear() { tasks_ = nullptr; list_.SetItemCount(0); selectedId_ = pressedId_ = draggedId_ = 0; }
std::optional<int> TaskListControl::RowAt(float y) const { return list_.RowIndexAt(y); }
bool TaskListControl::HitsRow(float x, float y) const noexcept {
    if (!visible_ || !tasks_ || tasks_->empty() || !HitTest(x, y)) return false;
    const std::optional<int> row = RowAt(y);
    return row.has_value() && *row >= 0 && *row < static_cast<int>(tasks_->size());
}
const Task* TaskListControl::TaskAt(float y) const { if (!tasks_) return nullptr; auto row = RowAt(y); return row && *row < static_cast<int>(tasks_->size()) ? &(*tasks_)[*row] : nullptr; }
D2D1_RECT_F TaskListControl::RowBounds(int index) const { const float y = bounds_.top + index * 74.0F - list_.Offset(); return {bounds_.left + 4, y + 4, bounds_.right - 4, y + 70}; }

std::string TaskListControl::CategoryName(const Task& task) const {
    if (!task.categoryId) return "未分类";
    for (const auto& category : categories_) if (category.category.id == *task.categoryId) return category.category.name;
    return "未知分类";
}

void TaskListControl::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    list_.SetViewport(bounds_);
    list_.EnsureVisibleRange();
    context.PushAxisAlignedClip(bounds_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!tasks_ || tasks_->empty()) {
        resources.DrawText(L"暂无任务", {bounds_.left, bounds_.top + 70, bounds_.right, bounds_.top + 104}, nullptr, true, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        resources.DrawText(L"在上方输入标题并按 Enter，或使用 Ctrl+N 新建", {bounds_.left, bounds_.top + 112, bounds_.right, bounds_.top + 142}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        context.PopAxisAlignedClip();
        return;
    }
    const auto [first, last] = list_.VisibleRange();
    for (int i = first; i < last && i < static_cast<int>(tasks_->size()); ++i) {
        const Task& task = (*tasks_)[i];
        D2D1_RECT_F row = RowBounds(i);
        if (task.id == draggedId_) row = {row.left + 8, dragY_ - 35, row.right + 8, dragY_ + 35};
        const float enter = AnimationClock::AnimationsEnabled() ? std::clamp(static_cast<float>((nowSeconds_ - enterAnimations_[task.id]) / 0.220), 0.0F, 1.0F) : 1.0F;
        row.top += (1.0F - enter) * 12.0F;
        row.bottom += (1.0F - enter) * 12.0F;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rowBrush;
        D2D1_COLOR_F rowColor = task.id == selectedId_ ? resources.Colors().accent : resources.Colors().panelStrong;
        if (task.id == selectedId_) rowColor.a = 0.12F;
        if (task.id == draggedId_) rowColor.a *= 0.65F;
        rowColor.a *= enter;
        context.CreateSolidColorBrush(rowColor, &rowBrush);
        resources.DrawRoundRect(row, 12, rowBrush.Get(), resources.BorderBrush());
        if (hoverRow_ == i && task.id != selectedId_ && task.id != draggedId_) {
            // Tint the whole row, not just the 删除 hint: the row itself has to react to the pointer.
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
            D2D1_COLOR_F hoverColor = resources.Colors().accent;
            hoverColor.a = 0.12F * enter;
            context.CreateSolidColorBrush(hoverColor, &hoverBrush);
            resources.DrawRoundRect(row, 12, hoverBrush.Get(), nullptr, 1.0F);
        }
        const float centerY = (row.top + row.bottom) / 2;
        const float checkProgress = AnimationClock::AnimationsEnabled() ? std::clamp(static_cast<float>((nowSeconds_ - completionAnimations_[task.id]) / 0.220), 0.0F, 1.0F) : 1.0F;
        const float checkScale = AnimationClock::EaseOutBack(checkProgress);
        D2D1_ELLIPSE check{{row.left + 24, centerY}, 10.0F * checkScale, 10.0F * checkScale};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> checkBrush;
        D2D1_COLOR_F checkColor = task.completed ? resources.Colors().accent : resources.Colors().panelStrong;
        context.CreateSolidColorBrush(checkColor, &checkBrush);
        context.FillEllipse(check, checkBrush.Get());
        context.DrawEllipse(check, task.completed ? resources.AccentBrush() : resources.MutedBrush(), 1.4F);
        if (task.completed) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white;
            context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);
            context.DrawLine({row.left + 19, centerY}, {row.left + 23, centerY + 4}, white.Get(), 1.8F);
            context.DrawLine({row.left + 23, centerY + 4}, {row.left + 30, centerY - 5}, white.Get(), 1.8F);
        }
        const D2D1_COLOR_F priorityColor = task.priority == Priority::High ? resources.Colors().danger : task.priority == Priority::Medium ? resources.Colors().warning : resources.Colors().success;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> priority;
        context.CreateSolidColorBrush(priorityColor, &priority);
        context.FillRoundedRectangle({{row.left + 43, row.top + 16, row.left + 47, row.bottom - 16}, 2, 2}, priority.Get());
        auto title = Dpi::Utf8ToWide(task.title);
        resources.DrawText(title, {row.left + 58, row.top + 8, row.right - 54, row.top + 36}, task.completed ? resources.MutedBrush() : resources.TextBrush());
        if (task.completed) {
            const float width = resources.MeasureTextWidth(title, row.right - row.left - 120);
            context.DrawLine({row.left + 58, row.top + 23}, {std::min(row.right - 54, row.left + 58 + width), row.top + 23}, resources.MutedBrush(), 1.0F);
        }
        std::string meta = CategoryName(task);
        if (task.dueAt) meta += "  ·  " + TimeRanges::DateString(*task.dueAt);
        if (task.completed) meta += "  ·  已完成";
        resources.DrawText(Dpi::Utf8ToWide(meta), {row.left + 58, row.top + 38, row.right - 54, row.bottom - 6}, resources.MutedBrush(), false, true);
        if (hoverRow_ == i && !readOnly_) resources.DrawText(L"删除", {row.right - 58, row.top + 12, row.right - 12, row.top + 36}, resources.DangerBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const float removedProgress = std::clamp(static_cast<float>((nowSeconds_ - removedAt_) / 0.180), 0.0F, 1.0F);
    if (removedProgress < 1.0F) for (const auto& [removed, originalIndex] : removedRows_) {
        const auto row = RowBounds(originalIndex);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ghost;
        auto color = resources.Colors().panelStrong;
        color.a *= 1.0F - removedProgress;
        context.CreateSolidColorBrush(color, &ghost);
        resources.DrawRoundRect(row, 12, ghost.Get());
        resources.DrawText(Dpi::Utf8ToWide(removed.title), {row.left + 58, row.top + 8, row.right - 54, row.top + 36}, resources.MutedBrush());
    } else removedRows_.clear();
    if (draggedId_ != 0) {
        const int target = std::clamp(static_cast<int>((dragY_ - bounds_.top + list_.Offset()) / 74.0F), 0, static_cast<int>(tasks_->size()));
        const float y = bounds_.top + target * 74.0F - list_.Offset();
        context.DrawLine({bounds_.left + 12, y}, {bounds_.right - 12, y}, resources.AccentBrush(), 2.0F);
    }
    if (list_.MaximumOffset() > 0) {
        const float track = bounds_.bottom - bounds_.top;
        const float thumb = std::max(28.0F, track * track / list_.ContentHeight());
        const float y = bounds_.top + (track - thumb) * list_.Offset() / list_.MaximumOffset();
        resources.DrawRoundRect({bounds_.right - 5, y, bounds_.right - 1, y + thumb}, 2, resources.MutedBrush());
    }
    context.PopAxisAlignedClip();
}

bool TaskListControl::OnMouseDown(float x, float y, UINT) {
    if (!HitTest(x, y) || !tasks_) return false;
    auto row = RowAt(y);
    if (!row) return false;
    const Task& task = (*tasks_)[*row];
    selectedId_ = pressedId_ = task.id;
    pressY_ = dragY_ = y;
    const auto bounds = RowBounds(*row);
    pressedAction_ = x <= bounds.left + 42 ? 1 : (hoverRow_ == *row && x >= bounds.right - 64 ? 2 : 0);
    return true;
}
bool TaskListControl::OnMouseMove(float x, float y) {
    Control::OnMouseMove(x, y);
    hoverRow_ = RowAt(y).value_or(-1);
    if (pressedId_ && canReorder_ && !readOnly_ && pressedAction_ == 0 && std::abs(y - pressY_) > 5.0F) draggedId_ = pressedId_;
    if (draggedId_) dragY_ = y;
    return true;
}
bool TaskListControl::OnMouseUp(float x, float y) {
    if (!pressedId_ || !tasks_) { draggedId_ = 0; return false; }
    auto it = std::find_if(tasks_->begin(), tasks_->end(), [this](const Task& task) { return task.id == pressedId_; });
    if (it == tasks_->end()) { pressedId_ = draggedId_ = 0; return true; }
    if (draggedId_ && reorder_) {
        int target = std::clamp(static_cast<int>((y - bounds_.top + list_.Offset()) / 74.0F), 0, static_cast<int>(tasks_->size()));
        const int original = static_cast<int>(std::distance(tasks_->begin(), it));
        target = std::clamp(target, 0, static_cast<int>(tasks_->size()));
        if (target != original && target != original + 1) {
            std::optional<std::int64_t> previous = target > 0 ? std::optional<std::int64_t>((*tasks_)[target - 1].id) : std::nullopt;
            std::optional<std::int64_t> next = target < static_cast<int>(tasks_->size()) ? std::optional<std::int64_t>((*tasks_)[target].id) : std::nullopt;
            reorder_(pressedId_, previous, next);
        }
    } else if (!readOnly_ && pressedAction_ == 1 && toggle_) toggle_(*it, !it->completed);
    else if (!readOnly_ && pressedAction_ == 2 && remove_) remove_(*it);
    pressedId_ = draggedId_ = 0;
    pressedAction_ = 0;
    return true;
}
bool TaskListControl::OnDoubleClick(float x, float y) { const Task* task = TaskAt(y); if (task && edit_ && !readOnly_) { edit_(*task); return true; } return false; }
bool TaskListControl::OnMouseWheel(float x, float y, int delta) { if (!HitTest(x, y)) return false; list_.ScrollByWheel(delta); if (hasNext_ && list_.Offset() >= list_.MaximumOffset() - 74 && loadMore_) loadMore_(); return true; }
bool TaskListControl::OnKeyDown(UINT key, bool, bool) {
    if (!focused_ || !tasks_ || tasks_->empty()) return false;
    if (key != VK_UP && key != VK_DOWN && key != VK_RETURN) return false;
    auto it = std::find_if(tasks_->begin(), tasks_->end(), [this](const Task& task) { return task.id == selectedId_; });
    int index = it == tasks_->end() ? 0 : static_cast<int>(std::distance(tasks_->begin(), it));
    if (key == VK_UP) index = std::max(0, index - 1);
    if (key == VK_DOWN) index = std::min(static_cast<int>(tasks_->size()) - 1, index + 1);
    selectedId_ = (*tasks_)[index].id;
    list_.ScrollTo(std::clamp(list_.Offset(), index * 74.0F - (bounds_.bottom - bounds_.top) + 74, index * 74.0F));
    if (key == VK_RETURN && edit_ && !readOnly_) edit_((*tasks_)[index]);
    return true;
}

}  // namespace todo::ui
