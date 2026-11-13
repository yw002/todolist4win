#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "core/TaskService.h"
#include "ui/controls/Button.h"
#include "ui/controls/PopupMenu.h"
#include "ui/controls/TextBox.h"

namespace todo::ui {

class SettingsDialog final : public Control {
public:
    // Overlays must start hidden. Control::visible_ defaults to true, so without this IsOpen()
    // would report true from the very first frame, Paint would draw the task/category card on top
    // of the navigation panel and MainWindow's WM_LBUTTONDOWN would swallow every click here.
    SettingsDialog() noexcept { visible_ = false; }
    enum class Mode { None, Task, Category, Connection };
    void ShowTask(const D2D1_RECT_F& window, const Task& task, const std::vector<CategoryCount>& categories, std::function<void(const EditTaskCommand&)> save, std::function<void()> cancel);
    void ShowCategory(const D2D1_RECT_F& window, const Category* category, std::function<void(std::int64_t, const std::string&, const std::string&)> save, std::function<void(std::int64_t)> remove, std::function<void()> cancel);
    // Data source settings (design 5.1). This is the minimal evolution of the old
    // (window, DatabaseConfig, save, cancel) form:
    //   * `draft` carries the current source in `kind` and the MySQL fields in `database`;
    //   * `draft.database.password` is NEVER displayed - the password box always starts empty and
    //     `hasSavedPassword` only decides the placeholder ("saved, leave empty to keep" vs "no saved
    //     password"). Nothing here can leak a secret back to the screen;
    //   * `localDatabasePath` is shown verbatim in local mode with an "open containing folder"
    //     button, because there is no UI to create/backup the local file.
    // The dialog reports what the user did as a PasswordAction; resolving it is SettingsService's
    // job, so an empty box can never be mistaken for "keep" anywhere else.
    void ShowConnection(const D2D1_RECT_F& window, const DataSourceDraft& draft, bool hasSavedPassword, std::wstring localDatabasePath, std::function<void(const DataSourceDraft&)> save, std::function<void()> cancel);
    void Hide() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;
    // Reports a failure that happened after the draft left the dialog (the switch probe and the
    // commit run asynchronously). The overlay stays open with the user's input intact.
    void SetError(const std::string& utf8Message);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    bool OnChar(wchar_t value) override;
    TextBox* ActiveTextBox() noexcept;
    // Cursor support: tells MainWindow which cursor the overlay wants at a DIP position.
    [[nodiscard]] bool HitTextField(float x, float y) noexcept;
    [[nodiscard]] bool HitClickable(float x, float y) noexcept;
private:
    void LayoutControls() noexcept;
    void Save();
    // Switches the connection card between the two sources and moves the focus off any field that
    // just got hidden (otherwise typing would go into an invisible TextBox).
    void SelectSource(DataSourceKind kind) noexcept;
    void OpenLocalFolder();
    std::vector<TextBox*> Fields() noexcept;
    std::vector<Control*> Clickables() noexcept;
    Mode mode_{Mode::None};
    TextBox field1_;
    TextBox field2_;
    TextBox field3_;
    TextBox field4_;
    TextBox field5_;
    Button save_;
    Button cancel_;
    Button priorityButton_;
    Button category_;
    Button remove_;
    Button sourceLocal_;
    Button sourceMySql_;
    Button openFolder_;
    Button clearPassword_;
    PopupMenu categoryMenu_;
    Control* pressed_{nullptr};
    Control* focused_{nullptr};
    Task task_;
    std::vector<CategoryCount> categories_;
    Priority priority_{Priority::Medium};
    std::optional<std::int64_t> categoryId_;
    std::int64_t editCategoryId_{0};
    // Connection state. `draft_` is the working copy the dialog edits; nothing in it is persisted
    // until the switch probe succeeds.
    DataSourceDraft draft_;
    bool hasSavedPassword_{false};
    bool passwordCleared_{false};
    std::wstring localPath_;
    // Card geometry is computed by LayoutControls() (not by Paint()), because the connection card is
    // taller than the task/category cards and its title has to clear the source selector row.
    D2D1_RECT_F card_{0, 0, 0, 0};
    std::function<void(const EditTaskCommand&)> saveTask_;
    std::function<void(std::int64_t, const std::string&, const std::string&)> saveCategory_;
    std::function<void(const DataSourceDraft&)> saveConnection_;
    std::function<void(std::int64_t)> removeCategory_;
    std::function<void()> cancelCallback_;
    std::wstring error_;
};

}  // namespace todo::ui
