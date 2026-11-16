#include "ui/SettingsDialog.h"

#include <d2d1_1helper.h>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "domain/TaskRules.h"
#include "domain/TimeRanges.h"
#include "ui/AppName.h"
#include "ui/Dpi.h"

namespace todo::ui {

namespace {

// One inset for every card: content used to end exactly on the card edge, so the fields and the action
// row visually pressed against the rounded border. 24 matches the title's own left margin.
constexpr float kCardInset = 24.0F;

// Section captions sit above full-width fields, all aligned to the same 24 DIP inset as the title; a
// single "属性" row replaces the stacked date / priority / category rows.
// Offsets are relative to card_.top and are shared by LayoutControls() and Paint().
// Task card. The outline is derived from the window instead of the old fixed 500x434 box: the rich
// body needs room for more than two text lines plus an image, so every extra DIP of window height
// goes to the body and the property row / action row keep their order underneath it.
constexpr float kTaskCardMinWidth = 500.0F;
constexpr float kTaskCardMaxWidth = 640.0F;
constexpr float kTaskCardMaxHeight = 620.0F;
constexpr float kTaskCardMargin = 48.0F;
constexpr float kTaskHeaderDivider = 54.0F;   // hairline under the dialog title
constexpr float kTaskTitleSection = 60.0F;
constexpr float kTaskBodySection = 126.0F;
constexpr float kTaskCaptionHeight = 18.0F;
constexpr float kTaskSectionToField = 22.0F;  // 18 DIP caption + 4 DIP breathing room
constexpr float kTaskTitleHeight = 36.0F;
constexpr float kTaskBodyMinHeight = 120.0F;
// Fixed vertical chrome around the body: 148 DIP above it (title row + captions) and 166 DIP below
// it (property row, reserved error strip, action row, bottom inset). bodyHeight = cardHeight - 314.
constexpr float kTaskFixedChrome = 314.0F;
constexpr float kTaskBodyToProperty = 34.0F;
constexpr float kTaskPropertyHeight = 36.0F;
constexpr float kTaskPropertyToAction = 38.0F;
constexpr float kTaskActionHeight = 40.0F;
constexpr float kTaskBottomInset = 18.0F;

// Category card. Two fields plus two buttons do not justify the old 500x462 outline, which left a
// ~250 DIP empty band between the last field and the action row. 440x264 keeps the same 24 DIP inset
// and a 40 DIP action row, only without the hole.
constexpr float kCategoryCardWidth = 440.0F;
constexpr float kCategoryCardHeight = 264.0F;

}  // namespace

std::vector<TextBox*> SettingsDialog::AllFields() noexcept { return {&field1_, &field2_, &field3_, &field4_, &field5_, &field6_}; }
std::vector<TextBox*> SettingsDialog::Fields() noexcept {
    // Task mode has only two editable text boxes; the settings card only edits the application name.
    // Hidden fields must never steal focus, IME input or hit tests while another card is open.
    if (mode_ == Mode::Task) return {&field1_, &field2_};
    if (mode_ == Mode::Settings) return {&field1_};
    return {&field1_, &field2_};
}
std::vector<Control*> SettingsDialog::Clickables() noexcept {
    // `bodyEditor_` is a Control but not a TextBox, so it joins this list only; Fields() keeps its
    // std::vector<TextBox*> signature. dueDateButton_ stays ahead of field3_ on purpose.
    // bodyEditor_ precedes field2_ so that, even if the legacy field were ever left visible over the
    // same rect, clicks and Ctrl+V still land on the rich editor.
    return {&field1_, &bodyEditor_, &field2_, &dueDateButton_, &field3_, &field4_, &field5_, &field6_,
            &save_, &cancel_, &priorityButton_, &category_, &remove_, &openFolder_};
}

void SettingsDialog::ShowTask(const D2D1_RECT_F& window, const Task& task, const std::vector<CategoryCount>& categories, std::function<void(const EditTaskCommand&)> save, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Task; task_ = task; categories_ = categories; priority_ = task.priority; categoryId_ = task.categoryId;
    saveTask_ = std::move(save); cancelCallback_ = std::move(cancel); saveCategory_ = nullptr; saveAppName_ = nullptr; removeCategory_ = nullptr;
    field1_.SetText(Dpi::Utf8ToWide(task.title)); field2_.SetText(Dpi::Utf8ToWide(task.note));
    field1_.SetPlaceholder(L"任务标题"); field2_.SetPlaceholder(L"正文");
    field2_.SetMultiline(true); field3_.SetPassword(false);
    // The rich body surface replaces field2_ in task mode. field2_ keeps only its rect (LayoutControls
    // copies it onto bodyEditor_); its visibility is turned off after the AllFields() loop below.
    bodyUnsupported_ = false;
    std::wstring bodyWarning;
    todo::BodyDocument document = todo::BodyDocument::FromLegacyNote(task.note);
    if (!task.bodyDoc.empty()) {
        auto parsed = todo::BodyDocument::Parse(task.bodyDoc);
        if (parsed.ok()) {
            document = parsed.value();
        } else {
            // §11 A12 / §12: a v>1 or malformed document is shown read-only; Save is refused so the
            // newer data is never overwritten by the legacy projection shown here.
            bodyUnsupported_ = true;
            bodyWarning = L"正文格式不受当前版本支持，保存已取消";
        }
    }
    bodyEditor_.Clear();
    bodyEditor_.SetDocument(document);
    bodyEditor_.SetEditorImageError(std::string());
    bodyEditor_.SetVisible(true);
    field2_.SetVisible(false);
    // The due date is a value, not free text: it arrives from the domain model and goes back as the
    // same type. Only the button label needs a formatted string.
    dueAt_ = task.dueAt;
    for (auto* field : AllFields()) field->SetVisible(field == &field1_ || field == &field2_);
    // Must run AFTER the loop above: it re-shows field2_, and a visible field2_ under the translucent
    // body surface would steal clicks and Ctrl+V (text paste) from the editor.
    field2_.SetVisible(false);
    bodyEditor_.SetVisible(true);
    dueDateButton_.SetVisible(true); datePicker_.Hide(); categoryMenu_.Hide();
    priorityButton_.SetVisible(true); category_.SetVisible(true); remove_.SetVisible(false);
    openFolder_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    priorityButton_.SetOnClick([this] { priority_ = priority_ == Priority::High ? Priority::Medium : priority_ == Priority::Medium ? Priority::Low : Priority::High; });
    dueDateButton_.SetOnClick([this] {
        if (datePicker_.IsOpen()) { datePicker_.Hide(); return; }
        ShowDatePicker();
    });
    datePicker_.SetOnPick([this](const std::wstring& value) {
        // An empty string is the calendar's "清空" action. Anything else is a validated date, so
        // the conversion cannot fail in practice; the guard keeps the invariant if it ever does.
        if (value.empty()) dueAt_.reset();
        else {
            const auto parsed = TimeRanges::ParseDueDate(Dpi::WideToUtf8(value));
            dueAt_ = parsed.ok() ? std::optional<LocalDateTime>(parsed.value()) : std::nullopt;
        }
        UpdateDueDateText();
    });
    category_.SetOnClick([this] {
        std::vector<PopupMenu::Item> items{{0, L"未分类"}};
        for (const auto& item : categories_) items.push_back({item.category.id, Dpi::Utf8ToWide(item.category.name)});
        // The property row sits further right than before, so keep the dropdown inside the window
        // both horizontally and (for long category lists) vertically.
        const float menuX = std::max(bounds_.left, std::min(category_.Bounds().left, bounds_.right - 220.0F));
        const float menuHeight = static_cast<float>(items.size()) * 36.0F + 8.0F;
        const float menuY = std::max(bounds_.top, std::min(category_.Bounds().bottom + 4, bounds_.bottom - menuHeight));
        categoryMenu_.Show(menuX, menuY, 220, std::move(items), [this](std::int64_t id) { categoryId_ = id == 0 ? std::nullopt : std::optional<std::int64_t>(id); });
    });
    UpdateDueDateText();
    error_.clear();
    if (!bodyWarning.empty()) error_ = bodyWarning;
    visible_ = true; focused_ = &field1_; field1_.OnFocus(true); LayoutControls(); BeginShow();
}

void SettingsDialog::ShowCategory(const D2D1_RECT_F& window, const Category* category, std::function<void(std::int64_t, const std::string&, const std::string&)> save, std::function<void(std::int64_t)> remove, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Category; editCategoryId_ = category ? category->id : 0;
    saveCategory_ = std::move(save); removeCategory_ = std::move(remove); cancelCallback_ = std::move(cancel); saveTask_ = nullptr; saveAppName_ = nullptr;
    field1_.SetText(category ? Dpi::Utf8ToWide(category->name) : L""); field2_.SetText(category ? Dpi::Utf8ToWide(category->color) : L"#007AFF");
    field1_.SetPlaceholder(L"分类名称"); field2_.SetPlaceholder(L"#RRGGBB");
    for (auto* field : AllFields()) field->SetVisible(field == &field1_ || field == &field2_);
    bodyEditor_.SetVisible(false);
    field2_.SetMultiline(false); field2_.SetPassword(false);
    dueDateButton_.SetVisible(false); datePicker_.Hide(); categoryMenu_.Hide();
    priorityButton_.SetVisible(false); category_.SetVisible(false); remove_.SetVisible(category != nullptr); remove_.SetDanger(true); remove_.SetText(L"删除分类");
    openFolder_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    remove_.SetOnClick([this] { if (removeCategory_) removeCategory_(editCategoryId_); });
    error_.clear(); visible_ = true; focused_ = &field1_; field1_.OnFocus(true); LayoutControls(); BeginShow();
}

void SettingsDialog::ShowSettings(const D2D1_RECT_F& window, std::wstring localDatabasePath, std::wstring appName, std::function<void(const std::string&)> saveAppName, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Settings;
    localPath_ = std::move(localDatabasePath);
    saveAppName_ = std::move(saveAppName); cancelCallback_ = std::move(cancel);
    saveTask_ = nullptr; saveCategory_ = nullptr; removeCategory_ = nullptr;
    appName_ = std::move(appName);
    field1_.SetText(appName_);
    field1_.SetPlaceholder(L"应用名称");
    for (auto* field : AllFields()) { field->SetVisible(field == &field1_); field->SetMultiline(false); field->SetPassword(false); }
    for (auto* field : {&field2_, &field3_, &field4_, &field5_, &field6_}) field->SetBounds({0, 0, 0, 0});
    bodyEditor_.SetVisible(false);
    dueDateButton_.SetVisible(false); datePicker_.Hide(); categoryMenu_.Hide();
    openFolder_.SetText(L"打开数据目录");
    openFolder_.SetOnClick([this] { OpenLocalFolder(); });
    openFolder_.SetVisible(true);
    priorityButton_.SetVisible(false); category_.SetVisible(false); remove_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    error_.clear(); visible_ = true; focused_ = &field1_; field1_.OnFocus(true); LayoutControls(); BeginShow();
}

void SettingsDialog::LayoutControls() noexcept {
    const float center = (bounds_.left + bounds_.right) / 2;
    // The task card grows with the window, the category card is deliberately compact (two fields do
    // not need 500x462) and the settings card keeps a 500 DIP label column.
    float cardWidth = mode_ == Mode::Category ? kCategoryCardWidth : 500.0F;
    float taskCardHeight = kTaskFixedChrome + kTaskBodyMinHeight;
    if (mode_ == Mode::Task) {
        cardWidth = std::clamp(bounds_.right - bounds_.left - kTaskCardMargin * 2.0F,
                               kTaskCardMinWidth, kTaskCardMaxWidth);
        const float availableHeight = bounds_.bottom - bounds_.top - kTaskCardMargin * 2.0F;
        const float bodyHeight = std::clamp(availableHeight - kTaskFixedChrome, kTaskBodyMinHeight,
                                            kTaskCardMaxHeight - kTaskFixedChrome);
        taskCardHeight = kTaskFixedChrome + bodyHeight;
    }
    const float left = center - cardWidth * 0.5F;
    const float right = center + cardWidth * 0.5F;
    // The settings card is 368 DIP tall, so its centre is 184 DIP above the window centre.
    float top = (bounds_.top + bounds_.bottom) / 2
              - (mode_ == Mode::Settings ? 184.0F : mode_ == Mode::Task ? taskCardHeight * 0.5F
                                                                      : kCategoryCardHeight * 0.5F);
    // Content stops one inset short of the card edge.
    const float fieldRight = right - kCardInset;
    if (mode_ == Mode::Settings) {
        field1_.SetBounds({left + 130, top + 82, fieldRight, top + 120});
        for (auto* field : {&field2_, &field3_, &field4_, &field5_, &field6_}) field->SetBounds({0, 0, 0, 0});
        openFolder_.SetBounds({left + 130, top + 192, fieldRight, top + 228});
        cancel_.SetBounds({right - kCardInset - 270, top + 312, right - kCardInset - 140, top + 352});
        save_.SetBounds({right - kCardInset - 128, top + 312, right - kCardInset, top + 352});
        card_ = {left, top, right, top + 368};
        return;
    }
    if (mode_ == Mode::Task) {
        // Every control starts at the same 24 DIP inset; the old right-aligned label column (which
        // forced fields to left + 130) is gone, replaced by section captions drawn in Paint().
        const float contentLeft = left + kCardInset;
        const float contentRight = right - kCardInset;
        const float field1Top = top + kTaskTitleSection + kTaskSectionToField;
        const float bodyTop = top + kTaskBodySection + kTaskSectionToField;
        const float bodyHeight = taskCardHeight - kTaskFixedChrome;
        const float bodyBottom = bodyTop + bodyHeight;
        const float propertyTop = bodyBottom + kTaskBodyToProperty;
        const float propertyBottom = propertyTop + kTaskPropertyHeight;
        const float actionTop = propertyBottom + kTaskPropertyToAction;
        field1_.SetBounds({contentLeft, field1Top, contentRight, field1Top + kTaskTitleHeight});
        field2_.SetBounds({contentLeft, bodyTop, contentRight, bodyBottom});
        // field2_ stays the single source of the body rect; the editor only mirrors it. The height
        // now follows the window (120 DIP minimum) instead of being pinned to 120.
        bodyEditor_.SetBounds(field2_.Bounds());
        // No placeholder rows for field3_/field4_/field5_: they are hidden in Task mode and their
        // old rects used to be the reason a ~92 DIP empty band sat above the action buttons.
        for (auto* field : {&field3_, &field4_, &field5_, &field6_}) field->SetBounds({0, 0, 0, 0});
        // Property row without a "clear date" ×: the calendar's own 清空 action is the single way to
        // drop a date. Date 168 + 8 gap + priority 92, and the category button absorbs the rest.
        dueDateButton_.SetBounds({contentLeft, propertyTop, contentLeft + 168.0F, propertyBottom});
        priorityButton_.SetBounds({contentLeft + 176, propertyTop, contentLeft + 268, propertyBottom});
        category_.SetBounds({contentLeft + 276, propertyTop, contentRight, propertyBottom});
        // Same spacing as the connection card: 40 DIP buttons, 12 DIP between cancel and save, and
        // 18 DIP of card below them. The reserved error strip (save.top - 34 .. -6) stays clear of
        // both the property row and the action row.
        remove_.SetBounds({left + kCardInset, actionTop, left + kCardInset + 150, actionTop + kTaskActionHeight});
        cancel_.SetBounds({right - kCardInset - 270, actionTop, right - kCardInset - 140, actionTop + kTaskActionHeight});
        save_.SetBounds({right - kCardInset - 128, actionTop, right - kCardInset, actionTop + kTaskActionHeight});
        card_ = {left, top, right, actionTop + kTaskActionHeight + kTaskBottomInset};
        return;
    }
    // Mode::Category: two fields, a reserved error line and one action row, nothing else.
    field1_.SetBounds({left + 100, top + 66, fieldRight, top + 104});
    field2_.SetBounds({left + 100, top + 120, fieldRight, top + 158});
    field3_.SetBounds({0, 0, 0, 0});
    field4_.SetBounds({0, 0, 0, 0});
    field5_.SetBounds({0, 0, 0, 0});
    field6_.SetBounds({0, 0, 0, 0});
    priorityButton_.SetBounds({0, 0, 0, 0});
    category_.SetBounds({0, 0, 0, 0});
    remove_.SetBounds({left + kCardInset, top + 206, left + kCardInset + 150, top + 246});
    cancel_.SetBounds({right - kCardInset - 270, top + 206, right - kCardInset - 140, top + 246});
    save_.SetBounds({right - kCardInset - 128, top + 206, right - kCardInset, top + 246});
    card_ = {left, top, right, top + kCategoryCardHeight};
}

void SettingsDialog::Hide() noexcept {
    if (focused_) focused_->OnFocus(false);
    focused_ = pressed_ = nullptr;
    categoryMenu_.Hide();
    datePicker_.Hide();
    // Paint() keeps using mode_/card_ during the close transition, so the state is only torn down
    // once the fade-out (see Tick) has finished.
    if (visible_ && mode_ != Mode::None && AnimationClock::AnimationsEnabled()) {
        closing_ = true;
        closingAt_ = animationClock_.Now();
        if (requestFrame_) requestFrame_();
        return;
    }
    FinishHide();
}

void SettingsDialog::FinishHide() noexcept {
    bodyEditor_.Clear();
    bodyEditor_.SetVisible(false);
    visible_ = false;
    closing_ = false;
    mode_ = Mode::None;
    focused_ = pressed_ = nullptr;
    categoryMenu_.Hide();
    datePicker_.Hide();
}

void SettingsDialog::BeginShow() noexcept {
    closing_ = false;
    shownAt_ = animationClock_.Now();
    if (requestFrame_) requestFrame_();
}

float SettingsDialog::Transition(double now) const noexcept {
    constexpr double kOpenSeconds = 0.24;
    constexpr double kCloseSeconds = 0.16;
    if (!AnimationClock::AnimationsEnabled()) return 1.0F;
    if (closing_) {
        const float t = static_cast<float>((now - closingAt_) / kCloseSeconds);
        return std::clamp(1.0F - t, 0.0F, 1.0F);
    }
    const float t = static_cast<float>((now - shownAt_) / kOpenSeconds);
    return std::clamp(t, 0.0F, 1.0F);
}

bool SettingsDialog::Tick(double now) noexcept {
    if (closing_) {
        if (now - closingAt_ >= 0.16) {
            FinishHide();
            return false;
        }
        return true;
    }
    if (!visible_) return false;
    return AnimationClock::AnimationsEnabled() && now - shownAt_ < 0.24;
}
bool SettingsDialog::IsOpen() const noexcept { return visible_; }
void SettingsDialog::ShowDatePicker() {
    const D2D1_RECT_F anchor = dueDateButton_.Bounds();
    // Show() fixes the calendar's view and bounds. Reading the size back from the control keeps
    // this dialog independent of DatePicker's private geometry constants.
    datePicker_.Show(anchor.left, anchor.bottom + 6.0F, dueAt_ ? Dpi::Utf8ToWide(TimeRanges::DateString(*dueAt_)) : std::wstring());
    const D2D1_RECT_F picker = datePicker_.Bounds();
    const float width = picker.right - picker.left;
    const float height = picker.bottom - picker.top;
    float x = std::min(anchor.left, bounds_.right - width);
    if (x < bounds_.left) x = bounds_.left;
    float y = picker.top;
    if (y + height > bounds_.bottom) {
        // A 336 DIP calendar rarely fits below a centred card: flip it above the field, and if even
        // that fails (a very short window) pin it to the bottom edge rather than clipping the grid.
        const float above = anchor.top - 6.0F - height;
        y = above >= bounds_.top ? above : std::max(bounds_.top, bounds_.bottom - height);
    }
    datePicker_.SetBounds({x, y, x + width, y + height});
}

void SettingsDialog::UpdateDueDateText() {
    dueDateButton_.SetText(dueAt_ ? L"截止 " + Dpi::Utf8ToWide(TimeRanges::DateString(*dueAt_)) : L"设置日期");
}

void SettingsDialog::SetError(const std::string& utf8Message) {
    // A failure reported after the card was submitted; it stays open so the user keeps their input.
    if (!visible_ || mode_ != Mode::Settings) return;
    error_ = Dpi::Utf8ToWide(utf8Message);
}
bool SettingsDialog::HitTextField(float x, float y) noexcept {
    if (!visible_) return false;
    // An open calendar owns the whole overlay: the fields behind it must not advertise an I-beam.
    if (datePicker_.IsOpen()) return false;
    for (auto* field : Fields()) if (field->Visible() && field->HitTest(x, y)) return true;
    // I-beam over the rich body surface as well (it is not in Fields()).
    if (mode_ == Mode::Task && bodyEditor_.Visible() && bodyEditor_.HitTest(x, y)) return true;
    return false;
}
bool SettingsDialog::HitClickable(float x, float y) noexcept {
    if (!visible_) return false;
    if (datePicker_.IsOpen()) return datePicker_.HitTest(x, y) || dueDateButton_.HitTest(x, y);
    if (categoryMenu_.IsOpen() && categoryMenu_.HitTest(x, y)) return true;
    for (auto* control : Clickables()) if (control->Visible() && control->HitTest(x, y)) return true;
    return false;
}

void SettingsDialog::OpenLocalFolder() {
    if (localPath_.empty()) { error_ = L"数据库文件路径不可用"; return; }
    const std::filesystem::path databasePath(localPath_);
    std::filesystem::path folder = databasePath.parent_path();
    if (folder.empty()) folder = databasePath;
    // On a fresh install the data directory may not exist yet; creating it here is what makes the
    // button usable *before* the first save, and it is the same directory the SQLite factory uses.
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        // Never silently do nothing, and never paste the folder into the message either: the path is
        // already spelled out two lines above the error strip, and a 60+ character path would overflow
        // the 452 DIP strip (the old wording measured 465 and lost its tail).
        error_ = L"无法自动打开数据目录，请按上方路径手动打开";
    } else {
        error_.clear();
    }
}

void SettingsDialog::Save() {
    if (mode_ == Mode::Task && saveTask_) {
        if (bodyUnsupported_) { error_ = L"正文格式不受当前版本支持，保存已取消"; return; }
        auto titleValid = TaskRules::ValidateTitle(Dpi::WideToUtf8(field1_.Text()));
        // note is only the pure-text projection: TaskService re-derives it from `body` anyway, which
        // is why the editor never mirrors field2_ and images never enter note.
        const todo::BodyDocument document = bodyEditor_.GetDocument();
        const std::string note = document.PlainText(false);
        auto noteValid = TaskRules::ValidateNote(note);
        if (!titleValid.ok() || !noteValid.ok()) { error_ = Dpi::Utf8ToWide(!titleValid.ok() ? titleValid.error().message : noteValid.error().message); return; }
        auto documentValid = document.Validate();
        if (!documentValid.ok()) { error_ = Dpi::Utf8ToWide(documentValid.error().message); return; }
        EditTaskCommand command;
        command.id = task_.id; command.expectedRevision = task_.revision; command.title = Dpi::WideToUtf8(field1_.Text()); command.note = note; command.priority = priority_; command.categoryId = categoryId_;
        // Newly pasted blobs travel with the edit; a save without body bytes leaves existing blobs
        // untouched (TaskService's contract).
        command.body = todo::BodyEdit{document.ToJson(), bodyEditor_.GetNewImages()};
        // The calendar already produced a domain date, so there is no text to parse and no format
        // error to surface. An absent value means "no due date" and leaves the optional empty.
        command.dueAt = dueAt_;
        saveTask_(command);
    } else if (mode_ == Mode::Category && saveCategory_) {
        const std::string name = Dpi::WideToUtf8(field1_.Text()), color = Dpi::WideToUtf8(field2_.Text());
        auto nameValid = TaskRules::ValidateCategoryName(name);
        auto colorValid = TaskRules::ValidateColor(color);
        if (!nameValid.ok() || !colorValid.ok()) { error_ = Dpi::Utf8ToWide(!nameValid.ok() ? nameValid.error().message : colorValid.error().message); return; }
        saveCategory_(editCategoryId_, name, color);
    }
    else if (mode_ == Mode::Settings && saveAppName_) {
        const auto trim = [](std::wstring value) {
            while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' || value.front() == L'\r' || value.front() == L'\n')) value.erase(value.begin());
            while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r' || value.back() == L'\n')) value.pop_back();
            return value;
        };
        const std::wstring appName = trim(field1_.Text());
        if (appName.empty()) { error_ = L"应用名称不能为空"; return; }
        if (appName.size() > kMaxAppNameLength) { error_ = L"应用名称不能超过 " + std::to_wstring(kMaxAppNameLength) + L" 个字符"; return; }
        if (appName != appName_) saveAppName_(Dpi::WideToUtf8(appName));
        appName_ = appName;
        error_.clear();
        auto callback = cancelCallback_;
        Hide();
        if (callback) callback();
    }
}

namespace {

// Every IDWriteTextFormat created by RenderResources uses DWRITE_WORD_WRAPPING_NO_WRAP, and
// D2D1_DRAW_TEXT_OPTIONS_CLIP then cuts whatever does not fit. Nothing here can rely on wrapping, so
// the local-source text is laid out as explicit lines instead of being handed a taller rectangle.

// MeasureTextWidth measures with the 14 DIP body format while the hint and folder lines are drawn
// with the 12 DIP secondary format. Glyph advances scale with the font size, so the measured width is
// scaled down before it is compared against the available width.
constexpr float kBodyToSecondary = 12.0F / 14.0F;

// The only place a Chinese hint may be broken without hurting legibility. The punctuation stays on
// the first line, which is what Chinese typography expects.
constexpr wchar_t kHintBreakPoints[] = L"、，。；：！？";

constexpr float kHintLineHeight = 20.0F;

struct HintLines {
    std::wstring first;
    std::wstring second;  // empty when the whole hint already fits on one line
};

[[nodiscard]] float SecondaryWidth(const RenderResources& resources, const std::wstring& text) {
    return resources.MeasureTextWidth(text) * kBodyToSecondary;
}

// Index where the suffix that keeps the last `keep` path components starts; 0 when the path has
// fewer components than that.
[[nodiscard]] std::size_t SuffixStart(const std::wstring& path, int keep) {
    int seen = 0;
    for (std::size_t i = path.size(); i > 0; --i) {
        if (path[i - 1] == L'\\' && ++seen == keep) return i;
    }
    return 0;
}

[[nodiscard]] int ComponentCount(const std::wstring& path) {
    if (path.empty()) return 0;
    int count = 1;
    for (wchar_t value : path) {
        if (value == L'\\') ++count;
    }
    return count;
}

/**
 * Fits a folder path into `available` DIP without ever needing a line break.
 *
 * A path contains no word-break opportunity, so an over-long one is clipped in place and the tail -
 * the only part that identifies the file - disappears. Whole leading components are dropped instead
 * and the cut is marked with "…\", never going below the last two components: "…\TodoListWin\data"
 * still tells the user where the file lives, whereas "…\data" would not.
 */
[[nodiscard]] std::wstring FitFolderLine(const RenderResources& resources, const std::wstring& folder, float available) {
    if (available <= 0.0F || SecondaryWidth(resources, folder) <= available) return folder;
    const int components = ComponentCount(folder);
    // Least aggressive first, so the longest suffix that still fits wins.
    for (int keep = components - 1; keep >= 2; --keep) {
        const std::wstring candidate = L"…\\" + folder.substr(SuffixStart(folder, keep));
        if (SecondaryWidth(resources, candidate) <= available) return candidate;
    }
    // Even the last two components overflow; keep them rather than showing a bare ellipsis.
    return L"…\\" + folder.substr(SuffixStart(folder, 2));
}

/**
 * Breaks a hint into at most two lines that each fit `available` DIP.
 *
 * Breaks are attempted after CJK punctuation and the most balanced candidate wins, because a first
 * line that is filled to the last pixel reads as cramped. If no punctuation break fits, the longest
 * fitting prefix is used. The caller draws each line into its own 20 DIP strip.
 */
[[nodiscard]] HintLines SplitIntoTwoLines(const RenderResources& resources, const std::wstring& text, float available) {
    if (SecondaryWidth(resources, text) <= available) return HintLines{text, std::wstring()};
    std::size_t best = std::wstring::npos;
    float bestSlack = -1.0F;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (std::wcschr(kHintBreakPoints, text[i - 1]) == nullptr) continue;
        const std::wstring head = text.substr(0, i);
        const std::wstring tail = text.substr(i);
        if (tail.empty()) continue;
        const float headWidth = SecondaryWidth(resources, head);
        const float tailWidth = SecondaryWidth(resources, tail);
        if (headWidth > available || tailWidth > available) continue;
        const float slack = available - std::max(headWidth, tailWidth);
        if (slack > bestSlack) { bestSlack = slack; best = i; }
    }
    if (best != std::wstring::npos) return HintLines{text.substr(0, best), text.substr(best)};
    std::size_t cut = 0;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        if (SecondaryWidth(resources, text.substr(0, i)) > available) break;
        cut = i;
    }
    if (cut == 0 || cut >= text.size()) return HintLines{text, std::wstring()};
    return HintLines{text.substr(0, cut), text.substr(cut)};
}

}  // namespace

void SettingsDialog::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    LayoutControls();
    // Overlay transition: the scrim fades while the card scales up from 94% with a slight overshoot
    // (the reverse on close). Layout is unchanged, so hit rects stay where the card will settle.
    const float progress = Transition(animationClock_.Now());
    const float closed = closing_ ? 1.0F - progress : 0.0F;
    const float scale = closing_ ? 1.0F - 0.035F * closed : 0.94F + 0.06F * AnimationClock::EaseOutBack(progress);
    const float fade = closing_ ? progress : std::min(1.0F, progress * 2.0F);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    context.CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.32F * fade), &dim);
    context.FillRectangle(bounds_, dim.Get());
    D2D1_MATRIX_3X2_F savedTransform{};
    context.GetTransform(&savedTransform);
    if (scale != 1.0F) {
        context.SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
            D2D1::Point2F((card_.left + card_.right) * 0.5F, (card_.top + card_.bottom) * 0.5F)));
    }
    resources.DrawGlassPanel(card_, 14, true);
    const std::wstring title = mode_ == Mode::Task ? L"编辑任务" : mode_ == Mode::Category ? (editCategoryId_ ? L"编辑分类" : L"新增分类") : L"设置";
    resources.DrawText(title, {card_.left + 24, card_.top + 16, card_.right - 24, card_.top + 50}, nullptr, true);
    auto label = [&](const TextBox& field, const std::wstring& text) { resources.DrawText(text, {card_.left + kCardInset, field.Bounds().top, field.Bounds().left - 8, field.Bounds().bottom}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING); };
    if (mode_ == Mode::Task) {
        const float contentLeft = card_.left + kCardInset;
        const float contentRight = card_.right - kCardInset;
        // Three visual groups (标题 / 正文 / 属性) instead of right-aligned grey labels: a short
        // accent rule marks each caption and two hairlines separate the groups.
        auto divider = [&](float offset) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
            if (SUCCEEDED(context.CreateSolidColorBrush(resources.Colors().border, &line))) {
                const float y = card_.top + offset;
                context.FillRectangle(D2D1_RECT_F{contentLeft, y, contentRight, y + 1.0F}, line.Get());
            }
        };
        auto section = [&](const wchar_t* text, float offset) {
            const float y = card_.top + offset;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent;
            if (SUCCEEDED(context.CreateSolidColorBrush(resources.Colors().accent, &accent))) {
                context.FillRectangle(D2D1_RECT_F{contentLeft, y + 3, contentLeft + 3, y + 15}, accent.Get());
            }
            resources.DrawText(text, {contentLeft + 11, y, contentRight, y + kTaskCaptionHeight}, resources.MutedBrush(), false, true);
        };
        divider(kTaskHeaderDivider);
        section(L"标题", kTaskTitleSection);
        section(L"正文", kTaskBodySection);
        // The divider and the "属性" caption follow the body's real bottom edge, so a taller window
        // grows the body instead of leaving a gap above the property row.
        const float bodyBottomOffset = bodyEditor_.Bounds().bottom - card_.top;
        divider(bodyBottomOffset + 6.0F);
        section(L"属性", bodyBottomOffset + 12.0F);
        priorityButton_.SetText(priority_ == Priority::High ? L"优先级 高" : priority_ == Priority::Medium ? L"优先级 中" : L"优先级 低");
        std::wstring category = L"未分类";
        if (categoryId_) for (const auto& item : categories_) if (item.category.id == *categoryId_) category = Dpi::Utf8ToWide(item.category.name);
        const std::wstring prefixed = L"分类 " + category;
        const float available = category_.Bounds().right - category_.Bounds().left - 16.0F;
        category_.SetText(resources.MeasureTextWidth(prefixed) <= available ? prefixed : category);
        UpdateDueDateText();
    } else if (mode_ == Mode::Category) { label(field1_, L"名称"); label(field2_, L"颜色"); }
    else {
        // Settings card: application name plus the real SQLite file path. The path and the hint are
        // drawn as explicit lines (every text format is NO_WRAP) so neither gets clipped.
        label(field1_, L"应用名称");
        const float fileTop = openFolder_.Bounds().top - 56.0F;  // 46 DIP of file rows + 10 DIP gap
        const float textLeft = card_.left + 130.0F;
        const float textRight = card_.right - kCardInset;
        resources.DrawText(L"数据库文件", {card_.left + kCardInset, fileTop, card_.left + 122, fileTop + 24}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (localPath_.empty()) {
            resources.DrawText(L"（路径不可用）", {textLeft, fileTop, textRight, fileTop + 46}, resources.TextBrush(), false, false);
        } else {
            const std::filesystem::path databasePath(localPath_);
            const std::wstring folder = FitFolderLine(resources, databasePath.parent_path().wstring(), textRight - textLeft);
            resources.DrawText(folder, {textLeft, fileTop, textRight, fileTop + 24}, resources.MutedBrush(), false, true);
            resources.DrawText(databasePath.filename().wstring(), {textLeft, fileTop + 24, textRight, fileTop + 46}, resources.TextBrush(), false, false);
        }
        const float hintTop = openFolder_.Bounds().bottom + 10.0F;
        const HintLines hint = SplitIntoTwoLines(resources, L"数据保存在本地 SQLite 文件，不会上传或同步。",
                                                 (card_.right - kCardInset) - (card_.left + kCardInset));
        const D2D1_RECT_F firstLine{card_.left + kCardInset, hintTop, card_.right - kCardInset, hintTop + kHintLineHeight};
        const D2D1_RECT_F secondLine{card_.left + kCardInset, hintTop + kHintLineHeight, card_.right - kCardInset, hintTop + kHintLineHeight * 2.0F};
        resources.DrawText(hint.first, firstLine, resources.MutedBrush(), false, true);
        if (!hint.second.empty()) resources.DrawText(hint.second, secondLine, resources.MutedBrush(), false, true);
    }
    for (auto* field : AllFields()) field->Paint(context, resources);
    // Body editor paints after the text fields but before the floating calendar: datePicker_ must stay
    // the last stroke so the popup is never covered by the card.
    bodyEditor_.Paint(context, resources);
    if (!error_.empty()) resources.DrawText(error_, {card_.left + kCardInset, save_.Bounds().top - 34, card_.right - kCardInset, save_.Bounds().top - 6}, resources.DangerBrush(), false, true);
    save_.SetText(L"保存");
    save_.SetAccent(true); cancel_.SetText(L"取消");
    save_.Paint(context, resources); cancel_.Paint(context, resources);
    priorityButton_.Paint(context, resources); category_.Paint(context, resources); remove_.Paint(context, resources);
    dueDateButton_.Paint(context, resources);
    openFolder_.Paint(context, resources);
    categoryMenu_.Paint(context, resources);
    // Last paint call: the calendar is a floating layer and must never be covered by the card or
    // by another popup.
    datePicker_.Paint(context, resources);
    context.SetTransform(savedTransform);
}

bool SettingsDialog::OnMouseDown(float x, float y, UINT flags) {
    if (!visible_ || closing_) return true;
    // An open calendar is the topmost layer. The anchor button is exempt from the "click outside
    // closes it" rule so its own click handler can toggle the calendar (see OnMouseUp).
    if (datePicker_.IsOpen()) {
        if (datePicker_.HitTest(x, y)) return true;
        if (!dueDateButton_.HitTest(x, y)) datePicker_.Hide();
    }
    if (categoryMenu_.IsOpen()) return true;
    Control* hit = nullptr;
    for (auto* control : Clickables()) if (control->Visible() && control->HitTest(x, y)) { hit = control; break; }
    pressed_ = hit;
    if (focused_ && focused_ != hit) focused_->OnFocus(false);
    for (auto* field : Fields()) if (hit == field) { focused_ = field; field->OnFocus(true); break; }
    // BodyEditor is not a TextBox, so it is focused through its own branch; it forwards focus to the
    // inner TextBox of the active block, which is what keeps IME working.
    if (hit == &bodyEditor_) { focused_ = &bodyEditor_; bodyEditor_.OnFocus(true); }
    // The "open data folder" button is not a text input, so the caret must leave the field instead of
    // typing into it.
    if (hit == &openFolder_) focused_ = nullptr;
    return hit ? hit->OnMouseDown(x, y, flags) : true;
}
bool SettingsDialog::OnMouseMove(float x, float y) {
    if (closing_) return true;
    for (auto* control : Clickables()) control->OnMouseMove(x, y);
    categoryMenu_.OnMouseMove(x, y);
    datePicker_.OnMouseMove(x, y);
    return visible_;
}
bool SettingsDialog::OnMouseUp(float x, float y) {
    if (datePicker_.IsOpen() && !dueDateButton_.HitTest(x, y)) {
        // Release a press that started on a button, or it would keep its "pressed" look forever.
        // Button::OnMouseUp only fires a click while the pointer is still over it.
        if (auto* pending = pressed_) { pressed_ = nullptr; pending->OnMouseUp(x, y); }
        return datePicker_.OnMouseUp(x, y);
    }
    if (categoryMenu_.IsOpen()) return categoryMenu_.OnMouseUp(x, y);
    if (!pressed_) return visible_;
    auto* control = pressed_;
    pressed_ = nullptr;
    control->OnMouseUp(x, y);
    return true;
}
bool SettingsDialog::OnKeyDown(UINT key, bool ctrl, bool shift) {
    if (!visible_ || closing_) return true;
    // ESC closes the calendar first; a second ESC closes the dialog.
    if (key == VK_ESCAPE) {
        if (datePicker_.IsOpen()) { datePicker_.Hide(); return true; }
        auto cb = cancelCallback_; Hide(); if (cb) cb(); return true;
    }
    if (ctrl && key == VK_RETURN) { Save(); return true; }
    if (datePicker_.IsOpen()) return datePicker_.OnKeyDown(key, ctrl, shift);
    // BodyEditor must see the key before the inner TextBox: TextBox::Paste() would swallow Ctrl+V
    // and paste only text, never the image path.
    if (mode_ == Mode::Task && focused_ == &bodyEditor_) {
        if (bodyEditor_.OnKeyDown(key, ctrl, shift)) {
            const std::string pasteError = bodyEditor_.TakeError();
            if (!pasteError.empty()) error_ = Dpi::Utf8ToWide(pasteError);
            return true;
        }
    }
    return focused_ && focused_->OnKeyDown(key, ctrl, shift);
}
bool SettingsDialog::OnChar(wchar_t value) {
    if (closing_) return true;
    // The calendar is modal: keystrokes must not land in the text boxes behind it.
    if (datePicker_.IsOpen()) return true;
    return focused_ && focused_->OnChar(value);
}
TextBox* SettingsDialog::ActiveTextBox() noexcept { if (datePicker_.IsOpen()) return nullptr; if (mode_ == Mode::Task && focused_ == &bodyEditor_) return bodyEditor_.ActiveTextBox(); for (auto* field : Fields()) if (focused_ == field) return field; return nullptr; }

void SettingsDialog::SetEditorImages(const std::vector<BodyImage>& images, std::string error) {
    if (mode_ != Mode::Task) return;
    bodyEditor_.SetImages(images);
    bodyEditor_.SetEditorImageError(std::move(error));
}

bool SettingsDialog::OnMouseWheel(float x, float y, int delta) {
    if (!visible_ || closing_ || datePicker_.IsOpen()) return false;
    if (mode_ != Mode::Task) return false;
    return bodyEditor_.OnMouseWheel(x, y, delta);
}

}  // namespace todo::ui
