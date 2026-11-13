#pragma once

#include <functional>
#include <optional>
#include <string>

#include "app/AppController.h"
#include "ui/controls/Button.h"
#include "ui/controls/DatePicker.h"
#include "ui/controls/PopupMenu.h"
#include "ui/controls/TaskListControl.h"
#include "ui/controls/TextBox.h"

namespace todo::ui {

class TaskPage final : public Control {
public:
    TaskPage(AppController& controller, std::function<void(const Task&)> editTask, std::function<void(const Task&)> deleteTask);
    void Layout() noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnDoubleClick(float x, float y) override;
    bool OnMouseWheel(float x, float y, int delta) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    void FocusNew();
    void FocusSearch();
    void SetTime(double nowSeconds) noexcept { nowSeconds_ = nowSeconds; }
    TextBox* ActiveTextBox() noexcept;
    Control* FocusedControl() noexcept;
    // Cursor support: distinguishes text fields from clickable list rows at a DIP position.
    [[nodiscard]] bool HitTextField(float x, float y) const noexcept;
    [[nodiscard]] bool HitRow(float x, float y) const noexcept;
    // True when the position sits on a button-like control of the "new task" row / filter bar.
    [[nodiscard]] bool HitHandCursor(float x, float y) const noexcept;
    void OnFocus(bool focused) override;
private:
    void SubmitNew();
    void UpdateCategoryText();
    void UpdateDueDateText();
    AppController& controller_;
    TextBox newTitle_;
    Button dueDate_;
    TextBox search_;
    Button priority_;
    Button category_;
    Button add_;
    Button filter_;
    Button priorityFilter_;
    Button display_;
    PopupMenu categoryMenu_;
    DatePicker datePicker_;
    TaskListControl list_;
    Control* pressed_{nullptr};
    Control* focused_{nullptr};
    Priority newPriority_{Priority::Medium};
    std::optional<std::int64_t> newCategoryId_;
    // Picked due date as "YYYY-MM-DD"; empty means "no due date yet".
    std::optional<std::wstring> dueDateValue_;
    double nowSeconds_{0.0};
    bool newSubmitted_{false};
    std::uint64_t submitWriteVersion_{0};
};

}  // namespace todo::ui
