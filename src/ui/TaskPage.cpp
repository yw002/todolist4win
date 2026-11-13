#include "ui/TaskPage.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/TaskRules.h"
#include "domain/TimeRanges.h"
#include "ui/Dpi.h"

namespace todo::ui {

TaskPage::TaskPage(AppController& controller, std::function<void(const Task&)> editTask, std::function<void(const Task&)> deleteTask) : controller_(controller) {
    newTitle_.SetPlaceholder(L"输入任务标题，Enter 快速新建");
    dueDate_.SetText(L"设置日期");
    search_.SetPlaceholder(L"搜索任务  Ctrl+F");
    priority_.SetText(L"中优先级");
    category_.SetText(L"未分类");
    add_.SetText(L"添加任务");
    add_.SetAccent(true);
    filter_.SetText(L"全部状态");
    priorityFilter_.SetText(L"全部优先级");
    display_.SetText(L"完成留底");
    newTitle_.SetOnSubmit([this] { SubmitNew(); });
    add_.SetOnClick([this] { SubmitNew(); });
    dueDate_.SetOnClick([this] {
        if (datePicker_.IsOpen()) { datePicker_.Hide(); return; }
        // Anchored to the button's bottom-left corner so it always drops out of the same control.
        datePicker_.Show(dueDate_.Bounds().left, dueDate_.Bounds().bottom + 4.0F, dueDateValue_ ? *dueDateValue_ : std::wstring());
    });
    datePicker_.SetOnPick([this](const std::wstring& value) {
        // An empty string means the "清空" button was used.
        if (value.empty()) dueDateValue_.reset(); else dueDateValue_ = value;
        UpdateDueDateText();
    });
    search_.SetOnChanged([this](const std::wstring& value) { controller_.SetKeyword(Dpi::WideToUtf8(value)); });
    priority_.SetOnClick([this] { newPriority_ = newPriority_ == Priority::High ? Priority::Medium : newPriority_ == Priority::Medium ? Priority::Low : Priority::High; priority_.SetText(newPriority_ == Priority::High ? L"高优先级" : newPriority_ == Priority::Medium ? L"中优先级" : L"低优先级"); });
    category_.SetOnClick([this] {
        std::vector<PopupMenu::Item> items{{0, L"未分类"}};
        for (const auto& item : controller_.State().categories) items.push_back({item.category.id, Dpi::Utf8ToWide(item.category.name)});
        categoryMenu_.Show(category_.Bounds().left, category_.Bounds().bottom + 4, 190, std::move(items), [this](std::int64_t id) { newCategoryId_ = id == 0 ? std::nullopt : std::optional<std::int64_t>(id); UpdateCategoryText(); });
    });
    filter_.SetOnClick([this] {
        auto& filter = controller_.State().filter;
        filter.completion = filter.completion == CompletionFilter::All ? CompletionFilter::Pending : filter.completion == CompletionFilter::Pending ? CompletionFilter::Completed : CompletionFilter::All;
        filter_.SetText(filter.completion == CompletionFilter::All ? L"全部状态" : filter.completion == CompletionFilter::Pending ? L"仅待办" : L"仅已完成");
        controller_.Navigate(controller_.State().route, controller_.State().categoryId);
    });
    priorityFilter_.SetOnClick([this] {
        auto& filter = controller_.State().filter;
        if (!filter.priority) filter.priority = Priority::High;
        else if (*filter.priority == Priority::High) filter.priority = Priority::Medium;
        else if (*filter.priority == Priority::Medium) filter.priority = Priority::Low;
        else filter.priority.reset();
        priorityFilter_.SetText(!filter.priority ? L"全部优先级" : *filter.priority == Priority::High ? L"高优先级" : *filter.priority == Priority::Medium ? L"中优先级" : L"低优先级");
        controller_.Navigate(controller_.State().route, controller_.State().categoryId);
    });
    display_.SetOnClick([this] { const bool archive = controller_.State().completionDisplay == CompletionDisplay::KeepVisible; controller_.SetCompletionDisplay(archive ? CompletionDisplay::Archive : CompletionDisplay::KeepVisible); display_.SetText(archive ? L"完成归档" : L"完成留底"); });
    list_.SetCallbacks(
        [this](const Task& task, bool value) { controller_.SetCompleted(task.id, value, task.revision); },
        editTask,
        deleteTask,
        [this](std::int64_t id, std::optional<std::int64_t> previous, std::optional<std::int64_t> next) { controller_.Reorder({id, previous, next}); },
        [this] { controller_.LoadMore(); });
}

void TaskPage::Layout() noexcept {
    const float left = bounds_.left + 16, right = bounds_.right - 16;
    const float width = right - left;
    newTitle_.SetBounds({left, bounds_.top + 12, left + width * 0.43F, bounds_.top + 52});
    dueDate_.SetBounds({left + width * 0.43F + 8, bounds_.top + 12, left + width * 0.65F, bounds_.top + 52});
    priority_.SetBounds({left + width * 0.65F + 8, bounds_.top + 12, left + width * 0.79F, bounds_.top + 52});
    category_.SetBounds({left + width * 0.79F + 8, bounds_.top + 12, right - 94, bounds_.top + 52});
    add_.SetBounds({right - 86, bounds_.top + 12, right, bounds_.top + 52});
    search_.SetBounds({left, bounds_.top + 62, left + width * 0.43F, bounds_.top + 100});
    filter_.SetBounds({left + width * 0.43F + 8, bounds_.top + 62, left + width * 0.60F, bounds_.top + 100});
    priorityFilter_.SetBounds({left + width * 0.60F + 8, bounds_.top + 62, left + width * 0.77F, bounds_.top + 100});
    display_.SetBounds({left + width * 0.77F + 8, bounds_.top + 62, left + width * 0.94F, bounds_.top + 100});
    list_.SetBounds({left, bounds_.top + 110, right, bounds_.bottom - 58});
}

void TaskPage::UpdateCategoryText() {
    std::wstring text = L"未分类";
    if (newCategoryId_) for (const auto& item : controller_.State().categories) if (item.category.id == *newCategoryId_) text = Dpi::Utf8ToWide(item.category.name);
    category_.SetText(text);
}

void TaskPage::UpdateDueDateText() {
    dueDate_.SetText(dueDateValue_ && !dueDateValue_->empty() ? L"截止 " + *dueDateValue_ : L"设置日期");
}

void TaskPage::SubmitNew() {
    if (controller_.State().readOnly) { controller_.ShowToast("数据库未连接，无法新建"); return; }
    CreateTaskCommand command;
    command.title = Dpi::WideToUtf8(newTitle_.Text());
    command.priority = newPriority_;
    command.categoryId = newCategoryId_;
    // The calendar already produces "YYYY-MM-DD", but the value is still validated here so a
    // corrupted / hand-edited state can never reach the service layer.
    if (dueDateValue_) {
        const std::string due = TaskRules::Trim(Dpi::WideToUtf8(*dueDateValue_));
        if (!due.empty()) {
            auto parsed = TimeRanges::ParseDueDate(due);
            if (!parsed.ok()) { controller_.ShowToast(parsed.error().message); return; }
            command.dueAt = parsed.value();
        }
    }
    submitWriteVersion_ = controller_.State().lastSuccessfulWrite;
    newSubmitted_ = true;
    controller_.CreateTask(command);
}

void TaskPage::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    // The calendar belongs to this page: leaving it open while the page is swapped out would make
    // it pop back up on the next visit, so it is closed together with the page.
    if (!visible_) { datePicker_.Hide(); return; }
    Layout();
    const auto& state = controller_.State();
    if (newSubmitted_ && state.lastSuccessfulWrite != submitWriteVersion_) { newSubmitted_ = false; newTitle_.SetText(L""); dueDateValue_.reset(); datePicker_.Hide(); UpdateDueDateText(); }
    const bool editable = !state.readOnly && state.connection == ConnectionState::Online;
    // A braced-init-list cannot hold mixed derived pointer types (auto deduction fails with C3535),
    // so build a properly typed array first and let each element convert to Control* on its own.
    Control* const editables[] = {&newTitle_, &dueDate_, &priority_, &category_, &add_, &filter_, &priorityFilter_, &display_};
    for (Control* control : editables) control->SetEnabled(editable || control == &search_);
    newTitle_.SetEnabled(editable); dueDate_.SetEnabled(editable); priority_.SetEnabled(editable); category_.SetEnabled(editable); add_.SetEnabled(editable);
    filter_.SetEnabled(true); priorityFilter_.SetEnabled(true); display_.SetEnabled(true);
    if (state.route == Route::Category && state.categoryId) newCategoryId_ = state.categoryId;
    UpdateCategoryText();
    list_.SetTasks(state.tasks);
    list_.SetCategories(state.categories);
    list_.SetReadOnly(!editable);
    const bool filtered = !state.filter.keyword.empty() || state.filter.priority.has_value() || state.filter.completion != CompletionFilter::All;
    list_.SetCanReorder(editable && !filtered && (state.route == Route::AllTasks || state.route == Route::Category));
    list_.SetHasNextPage(state.hasTaskNext);
    resources.DrawGlassPanel({bounds_.left + 8, bounds_.top + 4, bounds_.right - 8, bounds_.top + 106}, 14);
    newTitle_.Paint(context, resources); dueDate_.Paint(context, resources); search_.Paint(context, resources);
    priority_.Paint(context, resources); category_.Paint(context, resources); add_.Paint(context, resources); filter_.Paint(context, resources); priorityFilter_.Paint(context, resources); display_.Paint(context, resources);
    list_.SetTime(nowSeconds_);
    list_.Paint(context, resources);
    const D2D1_RECT_F footer{list_.Bounds().left, bounds_.bottom - 50, list_.Bounds().right, bounds_.bottom - 6};
    resources.DrawGlassPanel(footer, 12);
    const std::wstring overview = L"待办 " + std::to_wstring(state.stats.overview.pending) + L"     已完成 " + std::to_wstring(state.stats.overview.completed) + L"     逾期 " + std::to_wstring(state.stats.overview.overdue) + L"     今日完成 " + std::to_wstring(state.stats.overview.completedToday) + L"（按当前任务去重）";
    resources.DrawText(overview, {footer.left + 16, footer.top, footer.right - 16, footer.bottom}, resources.MutedBrush(), false, true);
    categoryMenu_.Paint(context, resources);
    // Must stay the last paint call: everything above draws its own glass panel and would
    // otherwise cover the calendar.
    datePicker_.Paint(context, resources);
}

bool TaskPage::OnMouseDown(float x, float y, UINT flags) {
    if (!visible_) return false;
    // An open calendar owns the pointer for the whole window; a click next to it closes it first.
    // The anchor button is exempt: it closes the calendar through its own click handler (see
    // OnMouseUp). Hiding it here would be undone by the Show() in Button::OnMouseUp, so clicking
    // the button twice would look like a dead toggle.
    if (datePicker_.IsOpen()) {
        if (datePicker_.HitTest(x, y)) return true;
        if (!dueDate_.HitTest(x, y)) datePicker_.Hide();
    }
    if (categoryMenu_.IsOpen()) return true;
    Control* hit = nullptr;
    Control* const clickable[] = {&newTitle_, &dueDate_, &search_, &priority_, &category_, &add_, &filter_, &priorityFilter_, &display_, &list_};
    for (Control* control : clickable) if (control->HitTest(x, y)) { hit = control; break; }
    pressed_ = hit;
    if (focused_ && focused_ != hit) focused_->OnFocus(false);
    if (hit == &newTitle_ || hit == &search_ || hit == &list_) { focused_ = hit; focused_->OnFocus(true); }
    return hit && hit->OnMouseDown(x, y, flags);
}
bool TaskPage::OnMouseMove(float x, float y) {
    bool handled = false;
    Control* const hoverables[] = {&newTitle_, &dueDate_, &search_, &priority_, &category_, &add_, &filter_, &priorityFilter_, &display_, &list_};
    for (Control* control : hoverables) handled = control->OnMouseMove(x, y) || handled;
    categoryMenu_.OnMouseMove(x, y);
    datePicker_.OnMouseMove(x, y);
    return handled;
}
bool TaskPage::OnMouseUp(float x, float y) {
    // The calendar is the topmost layer, so it consumes the release - unless the release landed on
    // the anchor button, which owns its own toggle (see OnMouseDown) so that a second click closes
    // the calendar again.
    if (datePicker_.IsOpen() && !dueDate_.HitTest(x, y)) {
        // Release a press that started on a button, otherwise it would keep its "pressed" look
        // forever. Button::OnMouseUp only fires a click while the pointer is still over it, so for
        // a release over the calendar this only clears the visual state.
        if (auto* pending = pressed_) { pressed_ = nullptr; pending->OnMouseUp(x, y); }
        return datePicker_.OnMouseUp(x, y);
    }
    if (categoryMenu_.IsOpen()) return categoryMenu_.OnMouseUp(x, y);
    auto* control = pressed_;
    pressed_ = nullptr;
    return control && control->OnMouseUp(x, y);
}
bool TaskPage::OnDoubleClick(float x, float y) { return list_.OnDoubleClick(x, y); }
bool TaskPage::OnMouseWheel(float x, float y, int delta) { return list_.OnMouseWheel(x, y, delta); }
bool TaskPage::OnKeyDown(UINT key, bool ctrl, bool shift) { if (datePicker_.IsOpen()) return datePicker_.OnKeyDown(key, ctrl, shift); return focused_ && focused_->OnKeyDown(key, ctrl, shift); }
void TaskPage::FocusNew() { if (focused_) focused_->OnFocus(false); focused_ = &newTitle_; newTitle_.OnFocus(true); }
void TaskPage::FocusSearch() { if (focused_) focused_->OnFocus(false); focused_ = &search_; search_.OnFocus(true); }
TextBox* TaskPage::ActiveTextBox() noexcept { return focused_ == &newTitle_ ? &newTitle_ : focused_ == &search_ ? &search_ : nullptr; }
Control* TaskPage::FocusedControl() noexcept { return focused_; }
bool TaskPage::HitTextField(float x, float y) const noexcept {
    if (!visible_) return false;
    for (const auto* field : {&newTitle_, &search_}) {
        if (field->Visible() && field->HitTest(x, y)) return true;
    }
    return false;
}
bool TaskPage::HitHandCursor(float x, float y) const noexcept {
    if (!visible_) return false;
    const Control* const buttons[] = {&dueDate_, &priority_, &category_, &add_, &filter_, &priorityFilter_, &display_};
    for (const Control* button : buttons) {
        if (button->Visible() && button->HitTest(x, y)) return true;
    }
    return false;
}
bool TaskPage::HitRow(float x, float y) const noexcept { return visible_ && list_.HitsRow(x, y); }
void TaskPage::OnFocus(bool focused) { Control::OnFocus(focused); if (!focused && focused_) focused_->OnFocus(false); }

}  // namespace todo::ui
