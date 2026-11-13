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
#include "ui/Dpi.h"

namespace todo::ui {

namespace {

// One inset for every card: content used to end exactly on the card edge, so the fields and the action
// row visually pressed against the rounded border. 24 matches the title's own left margin.
constexpr float kCardInset = 24.0F;

}  // namespace

std::vector<TextBox*> SettingsDialog::Fields() noexcept { return {&field1_, &field2_, &field3_, &field4_, &field5_}; }
std::vector<Control*> SettingsDialog::Clickables() noexcept {
    return {&field1_, &field2_, &field3_, &field4_, &field5_, &save_, &cancel_, &priorityButton_, &category_, &remove_,
            &sourceLocal_, &sourceMySql_, &openFolder_, &clearPassword_};
}

void SettingsDialog::ShowTask(const D2D1_RECT_F& window, const Task& task, const std::vector<CategoryCount>& categories, std::function<void(const EditTaskCommand&)> save, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Task; task_ = task; categories_ = categories; priority_ = task.priority; categoryId_ = task.categoryId;
    saveTask_ = std::move(save); cancelCallback_ = std::move(cancel); saveCategory_ = nullptr; saveConnection_ = nullptr; removeCategory_ = nullptr;
    field1_.SetText(Dpi::Utf8ToWide(task.title)); field2_.SetText(Dpi::Utf8ToWide(task.note)); field3_.SetText(task.dueAt ? Dpi::Utf8ToWide(TimeRanges::DateString(*task.dueAt)) : L"");
    field1_.SetPlaceholder(L"任务标题"); field2_.SetPlaceholder(L"备注"); field3_.SetPlaceholder(L"YYYY-MM-DD（留空无截止日期）");
    field2_.SetMultiline(true); field3_.SetPassword(false);
    for (auto* field : Fields()) field->SetVisible(field == &field1_ || field == &field2_ || field == &field3_);
    priorityButton_.SetVisible(true); category_.SetVisible(true); remove_.SetVisible(false);
    sourceLocal_.SetVisible(false); sourceMySql_.SetVisible(false); openFolder_.SetVisible(false); clearPassword_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    priorityButton_.SetOnClick([this] { priority_ = priority_ == Priority::High ? Priority::Medium : priority_ == Priority::Medium ? Priority::Low : Priority::High; });
    category_.SetOnClick([this] {
        std::vector<PopupMenu::Item> items{{0, L"未分类"}};
        for (const auto& item : categories_) items.push_back({item.category.id, Dpi::Utf8ToWide(item.category.name)});
        categoryMenu_.Show(category_.Bounds().left, category_.Bounds().bottom + 4, 220, std::move(items), [this](std::int64_t id) { categoryId_ = id == 0 ? std::nullopt : std::optional<std::int64_t>(id); });
    });
    error_.clear(); visible_ = true; focused_ = &field1_; field1_.OnFocus(true); LayoutControls();
}

void SettingsDialog::ShowCategory(const D2D1_RECT_F& window, const Category* category, std::function<void(std::int64_t, const std::string&, const std::string&)> save, std::function<void(std::int64_t)> remove, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Category; editCategoryId_ = category ? category->id : 0;
    saveCategory_ = std::move(save); removeCategory_ = std::move(remove); cancelCallback_ = std::move(cancel); saveTask_ = nullptr; saveConnection_ = nullptr;
    field1_.SetText(category ? Dpi::Utf8ToWide(category->name) : L""); field2_.SetText(category ? Dpi::Utf8ToWide(category->color) : L"#007AFF");
    field1_.SetPlaceholder(L"分类名称"); field2_.SetPlaceholder(L"#RRGGBB");
    for (auto* field : Fields()) field->SetVisible(field == &field1_ || field == &field2_);
    field2_.SetMultiline(false); field2_.SetPassword(false); priorityButton_.SetVisible(false); category_.SetVisible(false); remove_.SetVisible(category != nullptr); remove_.SetDanger(true); remove_.SetText(L"删除分类");
    sourceLocal_.SetVisible(false); sourceMySql_.SetVisible(false); openFolder_.SetVisible(false); clearPassword_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    remove_.SetOnClick([this] { if (removeCategory_) removeCategory_(editCategoryId_); });
    error_.clear(); visible_ = true; focused_ = &field1_; field1_.OnFocus(true); LayoutControls();
}

void SettingsDialog::ShowConnection(const D2D1_RECT_F& window, const DataSourceDraft& draft, bool hasSavedPassword, std::wstring localDatabasePath, std::function<void(const DataSourceDraft&)> save, std::function<void()> cancel) {
    bounds_ = window; mode_ = Mode::Connection;
    draft_ = draft;
    // The draft may arrive with a password left in it by a caller; the dialog never shows or keeps
    // one, so both the field and the draft's password slots start clean.
    draft_.database.password.clear();
    draft_.newPassword.clear();
    draft_.passwordAction = PasswordAction::Keep;
    hasSavedPassword_ = hasSavedPassword;
    passwordCleared_ = false;
    localPath_ = std::move(localDatabasePath);
    saveConnection_ = std::move(save); cancelCallback_ = std::move(cancel);
    saveTask_ = nullptr; saveCategory_ = nullptr; removeCategory_ = nullptr;

    field1_.SetText(Dpi::Utf8ToWide(draft_.database.host));
    field2_.SetText(std::to_wstring(draft_.database.port));
    field3_.SetText(Dpi::Utf8ToWide(draft_.database.username));
    // Deliberately empty: an already saved password is never displayed again (design 5.2). Whether
    // one exists is expressed by the placeholder, not by dots that the user could mistake for a
    // value they can edit or copy.
    field4_.SetText(L"");
    field5_.SetText(Dpi::Utf8ToWide(draft_.database.database));
    field1_.SetPlaceholder(L"主机"); field2_.SetPlaceholder(L"端口"); field3_.SetPlaceholder(L"用户名");
    field4_.SetPlaceholder(hasSavedPassword_ ? L"已保存密码，留空则沿用" : L"未保存密码，可留空");
    field5_.SetPlaceholder(L"数据库名");
    for (auto* field : Fields()) { field->SetVisible(true); field->SetMultiline(false); field->SetPassword(false); }
    field4_.SetPassword(true);

    sourceLocal_.SetText(L"本地数据库"); sourceMySql_.SetText(L"MySQL");
    sourceLocal_.SetOnClick([this] { SelectSource(DataSourceKind::Local); });
    sourceMySql_.SetOnClick([this] { SelectSource(DataSourceKind::MySql); });
    openFolder_.SetText(L"打开所在目录");
    openFolder_.SetOnClick([this] { OpenLocalFolder(); });
    clearPassword_.SetText(L"清除已保存密码");
    clearPassword_.SetDanger(true);
    clearPassword_.SetOnClick([this] {
        // Only a report of intent: nothing is removed until the switch probe succeeds and the draft
        // is committed. Typing a new password afterwards wins over this (Save() re-derives it).
        passwordCleared_ = true;
        field4_.SetText(L"");
        field4_.SetPlaceholder(L"保存后将清除已保存的密码");
    });
    priorityButton_.SetVisible(false); category_.SetVisible(false); remove_.SetVisible(false);
    save_.SetOnClick([this] { Save(); }); cancel_.SetOnClick([this] { auto cb = cancelCallback_; Hide(); if (cb) cb(); });
    error_.clear(); visible_ = true; SelectSource(draft_.kind);
}

void SettingsDialog::SelectSource(DataSourceKind kind) noexcept {
    draft_.kind = kind;
    const bool mySql = kind == DataSourceKind::MySql;
    // Switching to the local source hides every MySQL field; a field that keeps the focus would
    // swallow the keystrokes into an invisible TextBox.
    if (!mySql && focused_ && focused_ != &save_ && focused_ != &cancel_) {
        for (auto* field : Fields()) {
            if (focused_ == field) {
                field->OnFocus(false);
                focused_ = nullptr;
                break;
            }
        }
    }
    for (auto* field : Fields()) field->SetVisible(mySql);
    openFolder_.SetVisible(!mySql);
    // A "clear the saved password" action only makes sense for MySQL and only while one exists.
    clearPassword_.SetVisible(mySql && hasSavedPassword_);
    sourceLocal_.SetAccent(!mySql);
    sourceMySql_.SetAccent(mySql);
    if (mySql) { focused_ = &field1_; field1_.OnFocus(true); }
    LayoutControls();
}

void SettingsDialog::LayoutControls() noexcept {
    const float center = (bounds_.left + bounds_.right) / 2;
    const float left = center - 250, right = center + 250;
    // The connection card needs more room at the top: its title sits above the data source selector.
    float top = (bounds_.top + bounds_.bottom) / 2 - (mode_ == Mode::Connection ? 250.0F : 240.0F);
    // The local source carries three rows of content, so its card shrinks to 354 DIP (top + 20 ->
    // top + 374) and is re-centred instead of reserving the height the five MySQL fields need. The
    // card keeps its width, so switching the source only changes the height, never the column layout.
    const bool localCard = mode_ == Mode::Connection && draft_.kind == DataSourceKind::Local;
    if (localCard) top = (bounds_.top + bounds_.bottom) * 0.5F - 197.0F;
    // Content stops one inset short of the card edge; the card itself keeps its old outline.
    const float fieldRight = right - kCardInset;
    if (mode_ == Mode::Connection) {
        sourceLocal_.SetBounds({left + 130, top + 80, left + 250, top + 116});
        sourceMySql_.SetBounds({left + 258, top + 80, left + 352, top + 116});
        field1_.SetBounds({left + 130, top + 130, fieldRight, top + 168});
        field2_.SetBounds({left + 130, top + 186, fieldRight, top + 224});
        field3_.SetBounds({left + 130, top + 242, fieldRight, top + 280});
        field4_.SetBounds({left + 130, top + 298, fieldRight, top + 336});
        field5_.SetBounds({left + 130, top + 354, fieldRight, top + 392});
        // The local block is 148 DIP tall: 46 of file rows, 10, the 36 DIP button, 16, the 40 DIP hint.
        // On the short local card it starts right under the source selector; on the MySQL card the
        // button keeps its old position because the five fields are what is actually shown there.
        const float openFolderTop = localCard ? top + 182.0F : top + 196.0F;
        openFolder_.SetBounds({left + 130, openFolderTop, fieldRight, openFolderTop + 36});
        // The action row sits below a reserved error line (see Paint): 392 (last field) -> 398..424
        // error -> 430..470 buttons. Shifting it up would let the error text overlap a field. The
        // local card is shorter, so its reserved line moves with it (282..310).
        const float actionTop = localCard ? top + 316.0F : top + 430.0F;
        // 170 wide rather than 190: with a 24 DIP inset on both sides the card is only 500 DIP, so
        // "clear password" (190) and the cancel button that starts at right - 294 would overlap by
        // 8 DIP - and both are visible at once on the MySQL tab when a password is stored. The label
        // measures 98 DIP, so nothing is clipped by the narrower box.
        clearPassword_.SetBounds({left + kCardInset, actionTop, left + kCardInset + 170, actionTop + 40});
        cancel_.SetBounds({right - kCardInset - 270, actionTop, right - kCardInset - 140, actionTop + 40});
        save_.SetBounds({right - kCardInset - 128, actionTop, right - kCardInset, actionTop + 40});
        // Both source variants share the same width, so toggling the source never makes the card jump
        // sideways; only the height changes, and only because the local content is genuinely shorter.
        card_ = {left, top + 20, right, localCard ? top + 374.0F : save_.Bounds().bottom + 18.0F};
        return;
    }
    field1_.SetBounds({left + 130, top + 62, fieldRight, top + 100});
    field2_.SetBounds({left + 130, top + 116, fieldRight, mode_ == Mode::Task ? top + 206 : top + 154});
    field3_.SetBounds({left + 130, top + 220, fieldRight, top + 258});
    field4_.SetBounds({left + 130, top + 274, fieldRight, top + 312});
    field5_.SetBounds({left + 130, top + 328, fieldRight, top + 366});
    priorityButton_.SetBounds({left + 130, top + 274, left + 300, top + 312});
    category_.SetBounds({left + 312, top + 274, fieldRight, top + 312});
    remove_.SetBounds({left + kCardInset, top + 404, left + kCardInset + 150, top + 444});
    cancel_.SetBounds({right - kCardInset - 270, top + 404, right - kCardInset - 140, top + 444});
    save_.SetBounds({right - kCardInset - 128, top + 404, right - kCardInset, top + 444});
    card_ = {left, top, right, save_.Bounds().bottom + 18};
}

void SettingsDialog::Hide() noexcept { if (focused_) focused_->OnFocus(false); visible_ = false; mode_ = Mode::None; focused_ = pressed_ = nullptr; categoryMenu_.Hide(); }
bool SettingsDialog::IsOpen() const noexcept { return visible_; }
void SettingsDialog::SetError(const std::string& utf8Message) {
    // A switch failure is reported asynchronously, long after the draft left the dialog. Showing it
    // inside the card keeps the user in place with the fields they just typed still in front of them.
    if (!visible_ || mode_ != Mode::Connection) return;
    error_ = Dpi::Utf8ToWide(utf8Message);
}
bool SettingsDialog::HitTextField(float x, float y) noexcept {
    if (!visible_) return false;
    for (auto* field : Fields()) if (field->Visible() && field->HitTest(x, y)) return true;
    return false;
}
bool SettingsDialog::HitClickable(float x, float y) noexcept {
    if (!visible_) return false;
    if (categoryMenu_.IsOpen() && categoryMenu_.HitTest(x, y)) return true;
    for (auto* control : Clickables()) if (control->Visible() && control->HitTest(x, y)) return true;
    return false;
}

void SettingsDialog::OpenLocalFolder() {
    if (localPath_.empty()) { error_ = L"本地数据库路径不可用"; return; }
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
        auto titleValid = TaskRules::ValidateTitle(Dpi::WideToUtf8(field1_.Text()));
        auto noteValid = TaskRules::ValidateNote(Dpi::WideToUtf8(field2_.Text()));
        if (!titleValid.ok() || !noteValid.ok()) { error_ = Dpi::Utf8ToWide(!titleValid.ok() ? titleValid.error().message : noteValid.error().message); return; }
        EditTaskCommand command;
        command.id = task_.id; command.expectedRevision = task_.revision; command.title = Dpi::WideToUtf8(field1_.Text()); command.note = Dpi::WideToUtf8(field2_.Text()); command.priority = priority_; command.categoryId = categoryId_;
        const std::string due = TaskRules::Trim(Dpi::WideToUtf8(field3_.Text()));
        if (!due.empty()) { auto parsed = TimeRanges::ParseDueDate(due); if (!parsed.ok()) { error_ = Dpi::Utf8ToWide(parsed.error().message); return; } command.dueAt = parsed.value(); }
        saveTask_(command);
    } else if (mode_ == Mode::Category && saveCategory_) {
        const std::string name = Dpi::WideToUtf8(field1_.Text()), color = Dpi::WideToUtf8(field2_.Text());
        auto nameValid = TaskRules::ValidateCategoryName(name);
        auto colorValid = TaskRules::ValidateColor(color);
        if (!nameValid.ok() || !colorValid.ok()) { error_ = Dpi::Utf8ToWide(!nameValid.ok() ? nameValid.error().message : colorValid.error().message); return; }
        saveCategory_(editCategoryId_, name, color);
    }
    else if (mode_ == Mode::Connection && saveConnection_) {
        if (draft_.kind == DataSourceKind::MySql) {
            try {
                const std::string portText = TaskRules::Trim(Dpi::WideToUtf8(field2_.Text()));
                const int port = std::stoi(portText);
                if (port <= 0 || port > 65535) { error_ = L"端口必须在 1 到 65535 之间"; return; }
                draft_.database.port = static_cast<std::uint16_t>(port);
            } catch (...) { error_ = L"连接参数格式无效"; return; }
            draft_.database.host = TaskRules::Trim(Dpi::WideToUtf8(field1_.Text()));
            draft_.database.username = Dpi::WideToUtf8(field3_.Text());
            draft_.database.database = TaskRules::Trim(Dpi::WideToUtf8(field5_.Text()));
            if (draft_.database.host.empty() || draft_.database.database.empty()) { error_ = L"主机和数据库名不能为空"; return; }
            // Three explicit outcomes. An empty box means "keep" only because the user was told so by
            // the placeholder - it is never inferred from the string being empty somewhere else.
            const std::string typed = Dpi::WideToUtf8(field4_.Text());
            if (!typed.empty()) { draft_.passwordAction = PasswordAction::Replace; draft_.newPassword = typed; }
            else if (passwordCleared_) { draft_.passwordAction = PasswordAction::Clear; draft_.newPassword.clear(); }
            else { draft_.passwordAction = PasswordAction::Keep; draft_.newPassword.clear(); }
        } else {
            // Local target: the MySQL fields are hidden and untouched, and the settings layer ignores
            // the password action - the saved MySQL credential is kept for a later switch back.
            draft_.passwordAction = PasswordAction::Keep;
            draft_.newPassword.clear();
        }
        // The dialog's own copy never holds a plaintext password; the typed one lives in newPassword
        // only for the lifetime of the draft handed to the settings layer.
        draft_.database.password.clear();
        error_.clear();
        saveConnection_(draft_);
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
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    context.CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.32F), &dim);
    context.FillRectangle(bounds_, dim.Get());
    resources.DrawGlassPanel(card_, 14, true);
    const std::wstring title = mode_ == Mode::Task ? L"编辑任务" : mode_ == Mode::Category ? (editCategoryId_ ? L"编辑分类" : L"新增分类") : L"数据源设置";
    resources.DrawText(title, {card_.left + 24, card_.top + 16, card_.right - 24, card_.top + 50}, nullptr, true);
    auto label = [&](const TextBox& field, const std::wstring& text) { resources.DrawText(text, {card_.left + kCardInset, field.Bounds().top, field.Bounds().left - 8, field.Bounds().bottom}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING); };
    if (mode_ == Mode::Task) {
        label(field1_, L"标题"); label(field2_, L"备注"); label(field3_, L"截止日期");
        resources.DrawText(L"优先级", {card_.left + kCardInset, priorityButton_.Bounds().top, priorityButton_.Bounds().left - 8, priorityButton_.Bounds().bottom}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        priorityButton_.SetText(priority_ == Priority::High ? L"高" : priority_ == Priority::Medium ? L"中" : L"低");
        std::wstring category = L"未分类";
        if (categoryId_) for (const auto& item : categories_) if (item.category.id == *categoryId_) category = Dpi::Utf8ToWide(item.category.name);
        category_.SetText(category);
    } else if (mode_ == Mode::Category) { label(field1_, L"名称"); label(field2_, L"颜色"); }
    else if (draft_.kind == DataSourceKind::MySql) {
        resources.DrawText(L"数据源", {card_.left + kCardInset, sourceLocal_.Bounds().top, sourceLocal_.Bounds().left - 8, sourceLocal_.Bounds().bottom}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        label(field1_, L"主机"); label(field2_, L"端口"); label(field3_, L"用户名"); label(field4_, L"密码"); label(field5_, L"数据库");
    } else {
        // Local mode: no editable MySQL fields, only the real file path and a way to reveal it.
        // The path and the hint are laid out as explicit lines - see the helpers above - because a
        // taller rectangle would still render one clipped line (every format is NO_WRAP).
        // The file row is derived from the button instead of being stored, so Paint stays a pure
        // function of the bounds LayoutControls just computed.
        const float fileTop = openFolder_.Bounds().top - 56.0F;  // 46 DIP of file rows + 10 DIP gap
        const float textLeft = card_.left + 130.0F;
        const float textRight = card_.right - kCardInset;
        resources.DrawText(L"数据源", {card_.left + kCardInset, sourceLocal_.Bounds().top, sourceLocal_.Bounds().left - 8, sourceLocal_.Bounds().bottom}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        resources.DrawText(L"数据库文件", {card_.left + kCardInset, fileTop, card_.left + 122, fileTop + 24}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (localPath_.empty()) {
            resources.DrawText(L"（路径不可用）", {textLeft, fileTop, textRight, fileTop + 46}, resources.TextBrush(), false, false);
        } else {
            // Split so the file name gets its own line: it is what the user is looking for, and the
            // full path never fits the 354 DIP available to the right of the label.
            const std::filesystem::path databasePath(localPath_);
            const std::wstring folder = FitFolderLine(resources, databasePath.parent_path().wstring(), textRight - textLeft);
            resources.DrawText(folder, {textLeft, fileTop, textRight, fileTop + 24}, resources.MutedBrush(), false, true);
            resources.DrawText(databasePath.filename().wstring(), {textLeft, fileTop + 24, textRight, fileTop + 46}, resources.TextBrush(), false, false);
        }
        // Kept above the reserved error line so a failed switch never overlaps this hint.
        const float hintTop = openFolder_.Bounds().bottom + 16.0F;
        const HintLines hint = SplitIntoTwoLines(resources,
                                                 L"本地与 MySQL 的数据互相独立：切换只改变当前使用的数据库，不会复制、合并或删除任何一侧的数据。",
                                                 (card_.right - kCardInset) - (card_.left + kCardInset));
        const D2D1_RECT_F firstLine{card_.left + kCardInset, hintTop, card_.right - kCardInset, hintTop + kHintLineHeight};
        const D2D1_RECT_F secondLine{card_.left + kCardInset, hintTop + kHintLineHeight, card_.right - kCardInset, hintTop + kHintLineHeight * 2.0F};
        resources.DrawText(hint.first, firstLine, resources.MutedBrush(), false, true);
        if (!hint.second.empty()) resources.DrawText(hint.second, secondLine, resources.MutedBrush(), false, true);
    }
    for (auto* field : Fields()) field->Paint(context, resources);
    if (!error_.empty()) resources.DrawText(error_, {card_.left + kCardInset, save_.Bounds().top - 34, card_.right - kCardInset, save_.Bounds().top - 6}, resources.DangerBrush(), false, true);
    save_.SetText(mode_ == Mode::Connection && draft_.kind == DataSourceKind::MySql ? L"测试并保存" : L"保存");
    save_.SetAccent(true); cancel_.SetText(L"取消");
    save_.Paint(context, resources); cancel_.Paint(context, resources);
    priorityButton_.Paint(context, resources); category_.Paint(context, resources); remove_.Paint(context, resources);
    sourceLocal_.Paint(context, resources); sourceMySql_.Paint(context, resources);
    openFolder_.Paint(context, resources); clearPassword_.Paint(context, resources);
    categoryMenu_.Paint(context, resources);
}

bool SettingsDialog::OnMouseDown(float x, float y, UINT flags) {
    if (!visible_) return true;
    if (categoryMenu_.IsOpen()) return true;
    Control* hit = nullptr;
    for (auto* control : Clickables()) if (control->Visible() && control->HitTest(x, y)) { hit = control; break; }
    pressed_ = hit;
    if (focused_ && focused_ != hit) focused_->OnFocus(false);
    for (auto* field : Fields()) if (hit == field) { focused_ = field; field->OnFocus(true); break; }
    // The connection card's extra buttons (source selector, open folder, clear password) are not
    // text inputs, so the caret must leave the fields instead of typing into a hidden one.
    if (hit == &sourceLocal_ || hit == &sourceMySql_ || hit == &openFolder_ || hit == &clearPassword_) focused_ = nullptr;
    return hit ? hit->OnMouseDown(x, y, flags) : true;
}
bool SettingsDialog::OnMouseMove(float x, float y) { for (auto* control : Clickables()) control->OnMouseMove(x, y); categoryMenu_.OnMouseMove(x, y); return visible_; }
bool SettingsDialog::OnMouseUp(float x, float y) { if (categoryMenu_.IsOpen()) return categoryMenu_.OnMouseUp(x, y); if (!pressed_) return visible_; auto* control = pressed_; pressed_ = nullptr; control->OnMouseUp(x, y); return true; }
bool SettingsDialog::OnKeyDown(UINT key, bool ctrl, bool shift) {
    if (!visible_) return false;
    if (key == VK_ESCAPE) { auto cb = cancelCallback_; Hide(); if (cb) cb(); return true; }
    if (ctrl && key == VK_RETURN) { Save(); return true; }
    return focused_ && focused_->OnKeyDown(key, ctrl, shift);
}
bool SettingsDialog::OnChar(wchar_t value) {
    // Typing in the password box cancels a previous "clear" request: the newest intent wins.
    if (mode_ == Mode::Connection && focused_ == &field4_ && passwordCleared_) {
        passwordCleared_ = false;
        field4_.SetPlaceholder(L"新密码");
    }
    return focused_ && focused_->OnChar(value);
}
TextBox* SettingsDialog::ActiveTextBox() noexcept { for (auto* field : Fields()) if (focused_ == field) return field; return nullptr; }

}  // namespace todo::ui
