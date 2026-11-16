#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "core/TaskService.h"
#include "domain/Models.h"
#include "ui/AnimationClock.h"
#include "ui/controls/Button.h"
#include "ui/controls/BodyEditor.h"
#include "ui/controls/DatePicker.h"
#include "ui/controls/PopupMenu.h"
#include "ui/controls/TextBox.h"

namespace todo::ui {

class SettingsDialog final : public Control {
public:
    // Overlays must start hidden. Control::visible_ defaults to true, so without this IsOpen()
    // would report true from the very first frame, Paint would draw the task/category card on top
    // of the navigation panel and MainWindow's WM_LBUTTONDOWN would swallow every click here.
    SettingsDialog() noexcept { visible_ = false; }
    enum class Mode { None, Task, Category, Settings };
    void ShowTask(const D2D1_RECT_F& window, const Task& task, const std::vector<CategoryCount>& categories, std::function<void(const EditTaskCommand&)> save, std::function<void()> cancel);
    void ShowCategory(const D2D1_RECT_F& window, const Category* category, std::function<void(std::int64_t, const std::string&, const std::string&)> save, std::function<void(std::int64_t)> remove, std::function<void()> cancel);
    // Local storage settings. The application keeps everything in one SQLite file next to the user
    // profile; this card only shows that path, opens its folder and edits the window title name.
    void ShowSettings(const D2D1_RECT_F& window, std::wstring localDatabasePath, std::wstring appName, std::function<void(const std::string&)> saveAppName, std::function<void()> cancel);
    void Hide() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;
    // Advances the open/close transition (card scales up from 94% while the scrim fades in, and the
    // reverse on close). Returns true while another frame is needed; MainWindow keeps the frame
    // scheduler awake for as long as this (or ModalDialog::Tick) reports true.
    bool Tick(double now) noexcept;
    // Lets the owner schedule a frame when a transition starts or stops (MainWindow drives the
    // frame scheduler and would otherwise never learn that Hide() or a Show* began animating).
    void SetRequestFrame(std::function<void()> callback) { requestFrame_ = std::move(callback); }
    // Reports a failure after the card was submitted; the overlay stays open with the user's input.
    void SetError(const std::string& utf8Message);
    // Async image-load result for the body editor. Called from MainWindow after LoadBodyImages
    // completes; a null/empty list clears the previous task's pictures.
    void SetEditorImages(const std::vector<BodyImage>& images, std::string error);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnMouseWheel(float x, float y, int delta) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    bool OnChar(wchar_t value) override;
    TextBox* ActiveTextBox() noexcept;
    // Cursor support: tells MainWindow which cursor the overlay wants at a DIP position.
    [[nodiscard]] bool HitTextField(float x, float y) noexcept;
    [[nodiscard]] bool HitClickable(float x, float y) noexcept;
private:
    void LayoutControls() noexcept;
    // Starts the appear transition and cancels any in-flight close.
    void BeginShow() noexcept;
    // Actually clears the mode/editor state. Hide() defers this until the close transition ends.
    void FinishHide() noexcept;
    // 0 = fully closed, 1 = fully open. Linear; Paint() applies the easing.
    [[nodiscard]] float Transition(double now) const noexcept;
    void Save();
    // Opens the shared month calendar next to the due-date button, flipping above the row when the
    // window is too short. The calendar reports a validated "YYYY-MM-DD" (or an empty string).
    void ShowDatePicker();
    void UpdateDueDateText();
    // Opens the data directory that holds the SQLite file.
    void OpenLocalFolder();
    // Task mode uses the first two boxes (title and body), the settings card uses field1_ for the
    // application name, and the category card uses two. Fields() is mode-aware while AllFields()
    // stays the reset/visibility list.
    std::vector<TextBox*> AllFields() noexcept;
    std::vector<TextBox*> Fields() noexcept;
    std::vector<Control*> Clickables() noexcept;
    Mode mode_{Mode::None};
    TextBox field1_;
    TextBox field2_;
    TextBox field3_;
    TextBox field4_;
    TextBox field5_;
    // Application display name. It is edited in the settings card and persisted by MainWindow in
    // the user profile (app-name.txt).
    TextBox field6_;
    // Rich body surface: one field-like box that lays out text blocks and inline images. It replaces
    // field2_ in task mode; field2_ stays as the single-line fallback for the other cards.
    BodyEditor bodyEditor_;
    // Set when `body_doc` failed to parse because it was written by a newer schema: the content is
    // shown read-only and Save is refused instead of silently overwriting it (§11 A12).
    bool bodyUnsupported_{false};
    Button save_;
    Button cancel_;
    Button priorityButton_;
    Button category_;
    Button remove_;
    Button openFolder_;
    // field3_..field6_ are unused by the current cards; Task mode needs its own date/priority/category
    // controls instead of retyping those shared fields. The calendar popup is owned by this dialog too.
    Button dueDateButton_;
    DatePicker datePicker_;
    PopupMenu categoryMenu_;
    Control* pressed_{nullptr};
    Control* focused_{nullptr};
    Task task_;
    std::vector<CategoryCount> categories_;
    Priority priority_{Priority::Medium};
    std::optional<std::int64_t> categoryId_;
    // The picked due date, already converted to the domain type. The calendar guarantees a valid
    // "YYYY-MM-DD", so Save() never has to parse text or show a format error.
    std::optional<LocalDateTime> dueAt_;
    std::int64_t editCategoryId_{0};
    // Settings state: the SQLite path shown in the card. It is display-only.
    std::wstring localPath_;
    // Card geometry is computed by LayoutControls() (not by Paint()), because the card depends on the
    // window size and is centred on its own height.
    D2D1_RECT_F card_{0, 0, 0, 0};
    std::function<void(const EditTaskCommand&)> saveTask_;
    std::function<void(std::int64_t, const std::string&, const std::string&)> saveCategory_;
    std::function<void(const std::string&)> saveAppName_;
    std::wstring appName_;
    std::function<void(std::int64_t)> removeCategory_;
    std::function<void()> cancelCallback_;
    std::wstring error_;
    AnimationClock animationClock_;
    std::function<void()> requestFrame_;
    double shownAt_{0.0};
    double closingAt_{0.0};
    bool closing_{false};
};

}  // namespace todo::ui
