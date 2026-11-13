#include "ui/MainWindow.h"

#include <algorithm>
#include <cmath>

#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <memory>
#include <string>
#include <vector>
#include <windowsx.h>

#include "app/Win32UiDispatcher.h"
#include "config/AppPaths.h"
#include "resources/resource.h"
#include "ui/CursorFactory.h"
#include "ui/Dpi.h"

namespace todo {
namespace {
constexpr wchar_t kWindowClass[] = L"TodoListWin.MainWindow";
// One-shot retry timer used when the D3D/D2D device could not be rebuilt yet.
constexpr UINT_PTR kDeviceRetryTimer = 97;
// Client coordinate far outside any window, used to clear hover state on every control at once.
constexpr float kNoHover = -1.0e6F;
}

MainWindow::MainWindow(HINSTANCE instance, ConfigStore& configStore, AppConfig config)
    : instance_(instance), configStore_(configStore), config_(std::move(config)), scheduler_(nullptr), renderer_(nullptr) {}
MainWindow::~MainWindow() { tray_.Remove(); scheduler_.Stop(); renderer_.ReleaseDeviceResources(); }
HWND MainWindow::Handle() const noexcept { return hwnd_; }

bool MainWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    const UINT storedDpi = config_.window.lastDpi;
    const int initialLeft = Dpi::ToPixels(static_cast<float>(config_.window.leftDip), storedDpi);
    const int initialTop = Dpi::ToPixels(static_cast<float>(config_.window.topDip), storedDpi);
    const RECT initial = ClampToVisibleMonitor({initialLeft, initialTop, initialLeft + Dpi::ToPixels(static_cast<float>(config_.window.widthDip), storedDpi), initialTop + Dpi::ToPixels(static_cast<float>(config_.window.heightDip), storedDpi)});
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    // WS_EX_NOREDIRECTIONBITMAP is mandatory for a window whose whole client area is presented
    // through CreateSwapChainForComposition + IDCompositionTarget::SetRoot (see RenderEngine).
    // Without it DWM also allocates a redirection surface for the window, so DWM composition and
    // the swap chain fight over the same pixels: only a corner ends up painted while the rest of
    // the window stays invisible but still swallows every mouse message.
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, kWindowClass, L"TodoList for Windows", style, initial.left, initial.top, initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    // The window's own DPI is the only trustworthy DIP<->pixel factor: lastDpi comes from the ini
    // file and may describe another monitor, another scaling or an older session. Re-deriving the
    // placement from the stored DIP geometry with the real DPI keeps the physical position a pure
    // function of the configuration, so the saved DIP values can no longer be rescaled (and thus
    // drift) on every launch.
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = storedDpi;
    ApplyInitialPlacement();
    renderer_.Attach(hwnd_);
    scheduler_.Attach(hwnd_);
    if (!renderer_.CreateDeviceIndependentResources()) return false;
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!renderer_.CreateDeviceResources(client.right - client.left, client.bottom - client.top, dpi_)) return false;
    backdrop_.Enable(hwnd_);
    theme_.SetMode(config_.appearance.themeMode);
    // The controller is attached only after CreateWindow, so the remembered topmost flag must be
    // applied to the HWND directly here instead of going through SetTopmost (which needs controller_).
    ApplyTopmostToWindow(config_.window.topmost);
    trayAvailable_ = tray_.Create(hwnd_, wc.hIcon);
    ApplyRoundedCorners();
    if (config_.window.maximized) ShowWindow(hwnd_, SW_MAXIMIZE);
    Layout();
    return true;
}

void MainWindow::SetController(AppController& controller) {
    controller_ = &controller;
    taskPage_ = std::make_unique<ui::TaskPage>(controller, [this](const Task& task) { OpenTaskEditor(task); }, [this](const Task& task) { RequestDeleteTask(task); });
    historyPage_ = std::make_unique<ui::HistoryPage>(controller, [this](const std::vector<std::int64_t>& ids) { RequestDeleteHistory(ids); });
    statisticsPage_ = std::make_unique<ui::StatisticsPage>(controller);
    navigation_ = std::make_unique<ui::NavigationView>(controller);
    navigation_->SetOnEditCategory([this](std::int64_t id) { OpenCategoryEditor(id); });
    topmostButton_.SetText(L"置顶"); minimizeButton_.SetText(L"—"); maximizeButton_.SetText(L"□"); closeButton_.SetText(L"×"); themeButton_.SetText(L"主题"); connectionButton_.SetText(L"数据库"); retryButton_.SetText(L"重试");
    topmostButton_.SetOnClick([this] { SetTopmost(!controller_->State().topmost); });
    minimizeButton_.SetOnClick([this] { ShowWindow(hwnd_, SW_MINIMIZE); });
    maximizeButton_.SetOnClick([this] { ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); });
    closeButton_.SetOnClick([this] { SendMessageW(hwnd_, WM_CLOSE, 0, 0); });
    themeButton_.SetOnClick([this] { const bool dark = theme_.Palette().dark; controller_->SetThemeMode(dark ? ThemeMode::Light : ThemeMode::Dark); theme_.BeginTransition(controller_->State().themeMode, animationClock_.Now()); animationUntil_ = animationClock_.Now() + 0.35; RequestRender(); });
    connectionButton_.SetOnClick([this] { OpenConnectionSettings(); });
    for (auto* button : {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_}) button->SetRadius(17.0F);
    closeButton_.SetDanger(true);
    SetTimer(hwnd_, 99, 5000, nullptr);
    // Retrying reuses the open worker/factory. It must NOT go through a config round trip: the
    // in-memory config deliberately carries no password, so only a candidate switch can change the
    // connection parameters (design section 7).
    retryButton_.SetOnClick([this] { controller_->Retry(); });
    // Without a tray icon the window must stay reachable, so closing cannot hide it.
    if (!trayAvailable_) controller_->ShowToast("系统托盘不可用，关闭窗口将直接退出应用");
    Layout();
}

void MainWindow::ShowMainWindow() {
    if (!hwnd_) return;
    const UINT currentDpi = GetDpiForWindow(hwnd_);
    if (currentDpi != 0 && currentDpi != dpi_) {
        dpi_ = currentDpi;
        RECT client{}; GetClientRect(hwnd_, &client);
        renderer_.ResizeBuffers(client.right, client.bottom, dpi_);
        Layout();
    }
    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    backdrop_.Enable(hwnd_);
    ApplyRoundedCorners();
    SetForegroundWindow(hwnd_);
    RequestRender();
}

void MainWindow::ApplyInitialPlacement() noexcept {
    if (!hwnd_) return;
    const UINT placementDpi = dpi_ != 0 ? dpi_ : 96U;
    const int left = Dpi::ToPixels(static_cast<float>(config_.window.leftDip), placementDpi);
    const int top = Dpi::ToPixels(static_cast<float>(config_.window.topDip), placementDpi);
    const int width = Dpi::ToPixels(static_cast<float>(config_.window.widthDip), placementDpi);
    const int height = Dpi::ToPixels(static_cast<float>(config_.window.heightDip), placementDpi);
    const RECT target = ClampToVisibleMonitor({left, top, left + width, top + height});
    SetWindowPos(hwnd_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

RECT MainWindow::ClampToVisibleMonitor(const RECT& rect) const noexcept {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) return rect;
    RECT result = rect;
    const int width = std::min(rect.right - rect.left, monitor.rcWork.right - monitor.rcWork.left);
    const int height = std::min(rect.bottom - rect.top, monitor.rcWork.bottom - monitor.rcWork.top);
    result.left = std::clamp(rect.left, monitor.rcWork.left, monitor.rcWork.right - width);
    result.top = std::clamp(rect.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
    result.right = result.left + width;
    result.bottom = result.top + height;
    return result;
}

void MainWindow::ApplyRoundedCorners() noexcept {
    if (!hwnd_) return;
    constexpr DWORD kCornerPreference = 33;
    constexpr int kRound = 2;
    const HRESULT dwm = DwmSetWindowAttribute(hwnd_, kCornerPreference, &kRound, sizeof(kRound));
    if (SUCCEEDED(dwm)) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (IsZoomed(hwnd_)) { SetWindowRgn(hwnd_, nullptr, TRUE); return; }
    const int radius = Dpi::ToPixels(28.0F, dpi_);
    HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, radius, radius);
    if (region) SetWindowRgn(hwnd_, region, TRUE);
}
void MainWindow::ShowFromTray(bool newTask) { ShowMainWindow(); if (newTask) FocusNewTask(); }
void MainWindow::FocusNewTask() { if (!controller_) return; controller_->Navigate(Route::AllTasks); ShowMainWindow(); if (taskPage_) taskPage_->FocusNew(); }

D2D1_RECT_F MainWindow::WindowBounds() const noexcept {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return {0, 0, Dpi::ToDips(client.right, dpi_), Dpi::ToDips(client.bottom, dpi_)};
}

void MainWindow::Layout() noexcept {
    if (!hwnd_) return;
    const auto bounds = WindowBounds();
    const float width = bounds.right;
    topmostButton_.SetBounds({width - 326, 12, width - 288, 46});
    minimizeButton_.SetBounds({width - 202, 12, width - 168, 46});
    maximizeButton_.SetBounds({width - 160, 12, width - 126, 46});
    closeButton_.SetBounds({width - 118, 12, width - 84, 46});
    themeButton_.SetBounds({width - 280, 12, width - 210, 46});
    connectionButton_.SetBounds({width - 408, 12, width - 334, 46});
    retryButton_.SetBounds({width - 480, 12, width - 416, 46});
    if (navigation_) navigation_->SetBounds({16, 62, 208, bounds.bottom - 16});
    const D2D1_RECT_F content{220, 62, width - 16, bounds.bottom - 16};
    if (taskPage_) taskPage_->SetBounds(content);
    if (historyPage_) historyPage_->SetBounds(content);
    if (statisticsPage_) statisticsPage_->SetBounds(content);
}

void MainWindow::OnStateChanged() {
    if (!controller_) return;
    auto& state = controller_->State();
    const bool writeChanged = state.lastSuccessfulWrite != observedWrite_;
    if (writeChanged) {
        observedWrite_ = state.lastSuccessfulWrite;
        if (settings_.IsOpen() && state.overlay != Overlay::ConnectionSettings) { settings_.Hide(); state.overlay = Overlay::None; }
    }
    if (!state.toast.empty() && (state.toast != observedToast_ || writeChanged)) {
        observedToast_ = state.toast;
        toast_.Show(Dpi::Utf8ToWide(state.toast), animationClock_.Now());
    }
    // A pending data source switch finishes here. `Config().dataSource` only changes when the
    // candidate probe succeeded AND the configuration/credential were committed, so comparing it
    // with the requested target is an exact success test - no extra flag has to travel through
    // AppState. On failure the dialog stays open with the reason shown inside the card.
    if (switchTarget_ && !state.switching) {
        const bool committed = controller_->Config().dataSource == *switchTarget_;
        switchTarget_.reset();
        if (committed) {
            settings_.Hide();
            state.overlay = Overlay::None;
        } else {
            settings_.SetError(state.errorMessage);
        }
    }
    if (controller_->FlushPendingSearch(false)) SetTimer(hwnd_, 98, 50, nullptr);
    else KillTimer(hwnd_, 98);
    animationUntil_ = animationClock_.Now() + 0.30;
    RequestRender();
}

void MainWindow::Render() {
    if (!controller_) return;
    const double now = animationClock_.Now();
    // Device resources can disappear (driver reset, RDP session switch, GPU removal). Retry on a
    // 400 ms backoff instead of giving up, and keep asking for frames so the window never stays blank.
    if (!renderer_.HasDeviceResources()) {
        if (now >= nextDeviceRecovery_) {
            nextDeviceRecovery_ = now + 0.4;
            RecoverDeviceResources();
        }
        if (!renderer_.HasDeviceResources()) {
            // Arm the one-shot 400 ms retry exactly once; re-arming on every paint would keep
            // resetting the timer and could postpone recovery indefinitely.
            if (hwnd_ && !deviceRetryArmed_ && SetTimer(hwnd_, kDeviceRetryTimer, 400, nullptr) != 0) deviceRetryArmed_ = true;
            return;
        }
    }
    if (theme_.Update(now)) animationUntil_ = now + 0.05;
    renderer_.Resources().ApplyTheme(theme_.Palette());
    taskPage_->SetVisible(controller_->State().route != Route::History && controller_->State().route != Route::Statistics);
    historyPage_->SetVisible(controller_->State().route == Route::History);
    statisticsPage_->SetVisible(controller_->State().route == Route::Statistics);
    taskPage_->SetTime(now);
    statisticsPage_->SetTime(now);
    toast_.SetTime(now);
    renderer_.Render([&](ID2D1DeviceContext& context, RenderResources& resources) {
        const auto bounds = WindowBounds();
        const auto& palette = theme_.Palette();
        // The window body stays translucent on purpose: the swap chain is premultiplied and presented
        // through DirectComposition, so every pixel we leave below alpha 1 really shows the desktop
        // behind the window. A full-surface opaque wash (an earlier version lerped window ->
        // panelStrong across the whole client area) drove the composited alpha to ~0.95 and silently
        // cancelled that transparency.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
        context.CreateSolidColorBrush(palette.window, &background);
        context.FillRectangle(bounds, background.Get());
        // Warm accent bloom in the top-left corner plus a fainter one bottom-right. Both fade to
        // fully transparent, so they add depth without adding opacity.
        const auto bloom = [&](D2D1_POINT_2F center, float radius, float alpha) {
            const D2D1_GRADIENT_STOP stops[] = {{0.0F, D2D1::ColorF(palette.accent.r, palette.accent.g, palette.accent.b, alpha)}, {1.0F, D2D1::ColorF(palette.accent.r, palette.accent.g, palette.accent.b, 0.0F)}};
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> brush;
            if (FAILED(context.CreateGradientStopCollection(stops, 2, &collection))) return;
            const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES properties{center, {0.0F, 0.0F}, radius, radius};
            if (FAILED(context.CreateRadialGradientBrush(properties, collection.Get(), &brush))) return;
            context.FillRectangle(bounds, brush.Get());
        };
        bloom({bounds.right * 0.22F, bounds.top - bounds.bottom * 0.10F}, bounds.right * 0.75F, palette.dark ? 0.22F : 0.16F);
        bloom({bounds.right * 1.02F, bounds.bottom * 1.05F}, bounds.right * 0.62F, palette.dark ? 0.10F : 0.08F);
        resources.DrawRoundRect({16, 13, 46, 44}, 9, resources.AccentBrush());
        context.DrawLine({23, 29}, {29, 35}, resources.TextBrush(), 2.0F);
        context.DrawLine({29, 35}, {40, 22}, resources.TextBrush(), 2.0F);
        resources.DrawText(L"TodoList", {56, 10, 172, 48}, nullptr, true);
        // The status bar carries the active data source, so the user can always tell a local database
        // from a MySQL one without opening the settings (design section 7).
        const std::wstring sourcePrefix = controller_->State().dataSource == DataSourceKind::MySql ? L"MySQL · " : L"本地 · ";
        std::wstring status;
        if (controller_->State().switching) status = L"切换数据源中…";
        else if (controller_->State().loading) status = sourcePrefix + L"加载中";
        else if (controller_->State().connection == ConnectionState::Online) status = sourcePrefix + L"已连接";
        else if (controller_->State().readOnly) status = sourcePrefix + L"只读 · 数据可能不是最新";
        else status = sourcePrefix + L"连接中";
        // A missing tray icon must stay visible: it changes what the close button does.
        if (!trayAvailable_) status += L" · 托盘不可用";
        resources.DrawText(status, {180, 15, 420, 44}, resources.MutedBrush(), false, true);
        topmostButton_.SetAccent(controller_->State().topmost);
        topmostButton_.Paint(context, resources); minimizeButton_.Paint(context, resources); maximizeButton_.Paint(context, resources); closeButton_.Paint(context, resources); themeButton_.Paint(context, resources); connectionButton_.Paint(context, resources); retryButton_.Paint(context, resources);
        navigation_->Paint(context, resources);
        taskPage_->Paint(context, resources); historyPage_->Paint(context, resources); statisticsPage_->Paint(context, resources);
        if (!controller_->State().errorMessage.empty()) {
            // This used to sit at y=66..96, directly on top of the new-task input row, and it was
            // painted after taskPage_->Paint() so it covered the text box. Keep it inside the bottom
            // status strip, right-aligned, where nothing else is drawn.
            const D2D1_RECT_F error{bounds.right - 460, bounds.bottom - 50, bounds.right - 28, bounds.bottom - 14};
            resources.DrawText(Dpi::Utf8ToWide(controller_->State().errorMessage), error, resources.DangerBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        toast_.SetBounds(bounds);
        toast_.Paint(context, resources);
        settings_.Paint(context, resources);
        confirm_.Paint(context, resources);
    });
    if (now < animationUntil_ || toast_.Visible()) scheduler_.RequestFrame();
}
void MainWindow::RequestRender() { if (hwnd_) { InvalidateRect(hwnd_, nullptr, FALSE); scheduler_.RequestFrame(); } }

void MainWindow::OpenTaskEditor(const Task& task) {
    if (controller_->State().readOnly) return;
    controller_->State().overlay = Overlay::TaskEditor;
    settings_.ShowTask(WindowBounds(), task, controller_->State().categories, [this](const EditTaskCommand& command) { controller_->EditTask(command); }, [this] { controller_->State().overlay = Overlay::None; });
    RequestRender();
}
void MainWindow::OpenCategoryEditor(std::int64_t id) {
    if (controller_->State().readOnly) return;
    const Category* category = nullptr;
    if (id) for (const auto& item : controller_->State().categories) if (item.category.id == id) category = &item.category;
    controller_->State().overlay = Overlay::CategoryEditor;
    settings_.ShowCategory(WindowBounds(), category,
        [this](std::int64_t categoryId, const std::string& name, const std::string& color) { if (categoryId) controller_->UpdateCategory(categoryId, name, color); else controller_->CreateCategory(name, color); },
        [this](std::int64_t categoryId) {
            for (const auto& item : controller_->State().categories) if (item.category.id == categoryId && item.total > 0) { controller_->ShowToast("非空分类不能删除，请先迁移任务"); RequestRender(); return; }
            confirm_.Show(WindowBounds(), L"删除分类", L"删除分类不会删除历史快照，此操作不可撤销。", [this, categoryId](bool yes) { if (yes) { controller_->DeleteCategory(categoryId); settings_.Hide(); } });
        },
        [this] { controller_->State().overlay = Overlay::None; });
    RequestRender();
}
void MainWindow::RequestDeleteTask(const Task& task) {
    const std::wstring message = task.completed ? L"删除已完成任务会同时级联删除其历史事件，是否继续？" : L"删除此待办任务？此操作不可撤销。";
    confirm_.Show(WindowBounds(), L"删除任务", message, [this, id = task.id](bool yes) { if (yes) controller_->DeleteTask(id); });
    controller_->State().overlay = Overlay::Confirm;
}
void MainWindow::RequestDeleteHistory(const std::vector<std::int64_t>& ids) {
    confirm_.Show(WindowBounds(), L"永久删除历史", L"仅删除所选历史事件，不会修改当前任务。此操作不可撤销。", [this, ids](bool yes) { if (yes) controller_->DeleteHistory(ids); });
    controller_->State().overlay = Overlay::Confirm;
}
void MainWindow::OpenConnectionSettings() {
    if (controller_->State().switching) { controller_->ShowToast("正在切换数据源，请稍候"); return; }
    controller_->State().overlay = Overlay::ConnectionSettings;
    // The dialog never receives a password: only the source, the MySQL fields and whether a
    // credential exists. The local database path is resolved from AppPaths so the dialog can show
    // the real file and open its folder.
    DataSourceDraft draft;
    draft.kind = controller_->State().dataSource;
    draft.database = controller_->Config().database;
    draft.database.password.clear();
    draft.passwordAction = PasswordAction::Keep;
    const std::wstring localPath = AppPaths::Default().LocalDatabasePath().wstring();
    switchTarget_.reset();
    settings_.ShowConnection(WindowBounds(), draft, controller_->State().hasSavedCredential, localPath,
        [this](const DataSourceDraft& next) {
            // The switch is asynchronous: the probe runs on a candidate worker while this dialog
            // stays open. OnStateChanged() closes it once the commit has really happened.
            switchTarget_ = next.kind;
            controller_->RequestDataSourceSwitch(next);
        },
        [this] { controller_->State().overlay = Overlay::None; switchTarget_.reset(); });
    RequestRender();
}

bool MainWindow::RecoverDeviceResources() {
    if (!hwnd_) return false;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = client.right > 0 ? static_cast<UINT>(client.right) : 1U;
    const UINT height = client.bottom > 0 ? static_cast<UINT>(client.bottom) : 1U;
    if (!renderer_.RecoverDeviceResources(width, height, dpi_)) return false;
    nextDeviceRecovery_ = 0.0;
    if (deviceRetryArmed_) { KillTimer(hwnd_, kDeviceRetryTimer); deviceRetryArmed_ = false; }
    Layout();
    backdrop_.Enable(hwnd_);
    ApplyRoundedCorners();
    RequestRender();
    return true;
}

bool MainWindow::ApplyTopmostToWindow(bool value) noexcept {
    if (!hwnd_) return false;
    return SetWindowPos(hwnd_, value ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}

void MainWindow::SetTopmost(bool value) {
    if (!controller_) return;
    if (!ApplyTopmostToWindow(value)) {
        controller_->ShowToast("置顶设置被系统拒绝");
        return;
    }
    controller_->SetTopmost(value);
    config_ = controller_->Config();
    auto saved = configStore_.Save(config_);
    if (!saved.ok()) controller_->ShowToast(saved.error().message);
}
void MainWindow::SaveWindowConfig() {
    if (!hwnd_ || !controller_) return;
    // Ask the window for its DPI instead of trusting the cached one: the process can be moved to a
    // monitor with a different scale factor without WM_DPICHANGED reaching us yet, and a DPI that
    // disagrees with the rect would rescale the stored DIP values on the next launch.
    const UINT windowDpi = GetDpiForWindow(hwnd_);
    if (windowDpi != 0) dpi_ = windowDpi;
    RECT rect{};
    WINDOWPLACEMENT placement{sizeof(placement)};
    GetWindowPlacement(hwnd_, &placement);
    rect = placement.rcNormalPosition;
    config_ = controller_->Config();
    config_.window.leftDip = Dpi::ToDips(rect.left, dpi_);
    config_.window.topDip = Dpi::ToDips(rect.top, dpi_);
    config_.window.widthDip = Dpi::ToDips(rect.right - rect.left, dpi_);
    config_.window.heightDip = Dpi::ToDips(rect.bottom - rect.top, dpi_);
    config_.window.lastDpi = dpi_;
    config_.window.maximized = IsZoomed(hwnd_) != FALSE;
    (void)configStore_.Save(config_);
}

void MainWindow::ReleaseMouseCapture() noexcept {
    // A capture that outlives the button press keeps routing the whole desktop's mouse input to
    // this window, which the user experiences as "nothing can be clicked anywhere". Any path that
    // hides or destroys the window mid-drag therefore has to drop the capture and the press state.
    if (GetCapture() == hwnd_) ReleaseCapture();
    pressedTitle_ = nullptr;
}

ui::TextBox* MainWindow::ActiveTextBox() noexcept {
    if (settings_.IsOpen()) return settings_.ActiveTextBox();
    if (!controller_) return nullptr;
    if (controller_->State().route == Route::History && historyPage_) return historyPage_->ActiveTextBox();
    if (controller_->State().route != Route::Statistics && taskPage_) return taskPage_->ActiveTextBox();
    return nullptr;
}
ui::Control* MainWindow::ActivePageFocus() noexcept {
    if (settings_.IsOpen()) return &settings_;
    if (!controller_) return nullptr;
    if (controller_->State().route == Route::History && historyPage_) return historyPage_->FocusedControl();
    if (controller_->State().route != Route::Statistics && taskPage_) return taskPage_->FocusedControl();
    return nullptr;
}
float MainWindow::DipX(LPARAM lParam) const noexcept { return Dpi::ToDips(static_cast<short>(LOWORD(lParam)), dpi_); }
float MainWindow::DipY(LPARAM lParam) const noexcept { return Dpi::ToDips(static_cast<short>(HIWORD(lParam)), dpi_); }

HCURSOR MainWindow::ResolveCursor() noexcept {
    // The system arrow is only used as a fallback for the very first frame and if the custom
    // bitmap cannot be created; everything else in the client area gets a themed cursor.
    HCURSOR arrow = CursorFactory::Arrow(dpi_, theme_.Palette().accent);
    if (!arrow) arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (!hwnd_) return arrow;
    POINT point{};
    if (!GetCursorPos(&point)) return arrow;
    ScreenToClient(hwnd_, &point);
    const float x = Dpi::ToDips(point.x, dpi_);
    const float y = Dpi::ToDips(point.y, dpi_);
    HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
    HCURSOR ibeam = LoadCursorW(nullptr, IDC_IBEAM);
    // An open overlay owns the whole window, so its controls are consulted before anything behind.
    if (confirm_.IsOpen()) return confirm_.HitButton(x, y) ? hand : arrow;
    if (settings_.IsOpen()) {
        if (settings_.HitTextField(x, y)) return ibeam;
        if (settings_.HitClickable(x, y)) return hand;
        return arrow;
    }
    ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &connectionButton_, &retryButton_};
    for (auto* button : titleButtons) {
        if (button->Visible() && button->HitTest(x, y)) return hand;
    }
    if (navigation_ && navigation_->HitTest(x, y)) return hand;
    if (taskPage_ && taskPage_->Visible() && taskPage_->HitTest(x, y)) {
        if (taskPage_->HitTextField(x, y)) return ibeam;
        if (taskPage_->HitHandCursor(x, y)) return hand;
        if (taskPage_->HitRow(x, y)) return hand;
    }
    if (historyPage_ && historyPage_->Visible() && historyPage_->HitTest(x, y)) {
        if (historyPage_->HitTextField(x, y)) return ibeam;
        if (historyPage_->HitRow(x, y)) return hand;
    }
    return arrow;
}

void MainWindow::ClearHoverState() noexcept {
    mouseTracking_ = false;
    if (navigation_) navigation_->ClearHover();
    ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &connectionButton_, &retryButton_};
    for (auto* button : titleButtons) button->OnMouseMove(kNoHover, kNoHover);
    if (settings_.IsOpen()) settings_.OnMouseMove(kNoHover, kNoHover);
    if (taskPage_) taskPage_->OnMouseMove(kNoHover, kNoHover);
    if (historyPage_) historyPage_->OnMouseMove(kNoHover, kNoHover);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCCALCSIZE:
            // wParam == TRUE means the window is really being resized/moved and the client area has
            // to be recomputed: keeping the proposed rect gives the frameless client area the full
            // window rectangle. With wParam == FALSE the caller only queries the current client
            // rect, so the message must reach DefWindowProcW; answering 0 there would desynchronise
            // the client rect from the window rect and skew every later geometry calculation.
            if (wParam == TRUE) return 0;
            break;
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {Dpi::ToPixels(760, dpi_), Dpi::ToPixels(560, dpi_)};
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            RECT client{}; GetClientRect(hwnd_, &client);
            if (!IsZoomed(hwnd_)) {
                const int border = Dpi::ToPixels(6, dpi_);
                const bool left = point.x < border, right = point.x >= client.right - border, top = point.y < border, bottom = point.y >= client.bottom - border;
                if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT; if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT; if (right) return HTRIGHT; if (top) return HTTOP; if (bottom) return HTBOTTOM;
            }
            if (point.y < Dpi::ToPixels(54, dpi_) && point.x < client.right - Dpi::ToPixels(490, dpi_)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SIZE:
            // ResizeBuffers also creates the device resources when they are currently missing,
            // so this must not be skipped just because HasDeviceResources() is false.
            if (wParam != SIZE_MINIMIZED) renderer_.ResizeBuffers(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), dpi_);
            Layout(); ApplyRoundedCorners(); RequestRender(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{}; GetClientRect(hwnd_, &client);
            if (!renderer_.ResizeBuffers(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), dpi_)) RecoverDeviceResources();
            Layout(); ApplyRoundedCorners(); backdrop_.Invalidate(); RequestRender(); return 0;
        }
        case WM_DISPLAYCHANGE: if (!renderer_.HasDeviceResources()) RecoverDeviceResources(); else renderer_.HandleDeviceLost(); RequestRender(); return 0;
        case WM_SETTINGCHANGE: theme_.RefreshSystemTheme(); RequestRender(); return 0;
        case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd_, &paint); Render(); EndPaint(hwnd_, &paint); return 0; }
        case WM_TIMER:
            if (wParam == kDeviceRetryTimer) { KillTimer(hwnd_, kDeviceRetryTimer); deviceRetryArmed_ = false; Render(); return 0; }
            if (scheduler_.OnTimer(wParam)) { Render(); return 0; }
            if (wParam == 98 && controller_) { if (controller_->FlushPendingSearch(false)) SetTimer(hwnd_, 98, 50, nullptr); return 0; }
            if (wParam == 99 && controller_) { controller_->Tick(); return 0; }
            break;
        case WM_MOUSELEAVE:
            // Without this the last hovered row/button would stay highlighted after the pointer left.
            ClearHoverState();
            RequestRender();
            return 0;
        case WM_SETCURSOR:
            // Only the client area uses the custom cursors. Every non-client hit (resize borders,
            // menu, caption, scroll bars of the system) must keep the system-provided cursor, so the
            // message falls through to DefWindowProcW.
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(ResolveCursor());
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd_); SetFocus(hwnd_);
            const float x = DipX(lParam), y = DipY(lParam);
            if (confirm_.IsOpen()) { confirm_.OnMouseDown(x, y, static_cast<UINT>(wParam)); return 0; }
            if (settings_.IsOpen()) { settings_.OnMouseDown(x, y, static_cast<UINT>(wParam)); RequestRender(); return 0; }
            ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &connectionButton_, &retryButton_};
            for (auto* button : titleButtons) if (button->HitTest(x, y)) { pressedTitle_ = button; button->OnMouseDown(x, y, static_cast<UINT>(wParam)); RequestRender(); return 0; }
            if (navigation_->HitTest(x, y)) navigation_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else if (taskPage_->Visible()) taskPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else if (historyPage_->Visible()) historyPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else statisticsPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            RequestRender(); return 0;
        }
        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                // Register for a single WM_MOUSELEAVE so hover state can be cleared when the pointer
                // leaves the window; the registration has to be renewed after every leave.
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
                mouseTracking_ = TrackMouseEvent(&track) != FALSE;
            }
            const float x = DipX(lParam), y = DipY(lParam);
            if (confirm_.IsOpen()) break;
            if (settings_.IsOpen()) settings_.OnMouseMove(x, y);
            else {
                ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &connectionButton_, &retryButton_};
                for (auto* button : titleButtons) button->OnMouseMove(x, y);
                navigation_->OnMouseMove(x, y); taskPage_->OnMouseMove(x, y); historyPage_->OnMouseMove(x, y);
            }
            RequestRender(); return 0;
        }
        case WM_LBUTTONUP: {
            // Take the pressed control before releasing the capture: ReleaseCapture() synchronously
            // sends WM_CAPTURECHANGED, and the handler for that message clears pressedTitle_, which
            // would otherwise swallow the click of every title-bar button.
            ui::Control* const pressedTitle = pressedTitle_;
            pressedTitle_ = nullptr;
            ReleaseCapture();
            const float x = DipX(lParam), y = DipY(lParam);
            if (confirm_.IsOpen()) confirm_.OnMouseUp(x, y);
            else if (settings_.IsOpen()) settings_.OnMouseUp(x, y);
            else if (pressedTitle) pressedTitle->OnMouseUp(x, y);
            else if (navigation_->HitTest(x, y)) navigation_->OnMouseUp(x, y);
            else if (taskPage_->Visible()) taskPage_->OnMouseUp(x, y);
            else if (historyPage_->Visible()) historyPage_->OnMouseUp(x, y);
            else statisticsPage_->OnMouseUp(x, y);
            RequestRender(); return 0;
        }
        case WM_LBUTTONDBLCLK: { const float x = DipX(lParam), y = DipY(lParam); if (!settings_.IsOpen() && navigation_->HitTest(x, y)) navigation_->OnDoubleClick(x, y); else if (!settings_.IsOpen() && taskPage_->Visible()) taskPage_->OnDoubleClick(x, y); RequestRender(); return 0; }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(hwnd_, &point);
            const float x = Dpi::ToDips(point.x, dpi_), y = Dpi::ToDips(point.y, dpi_);
            if (!settings_.IsOpen()) { if (taskPage_->Visible()) taskPage_->OnMouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam)); else if (historyPage_->Visible()) historyPage_->OnMouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam)); }
            RequestRender(); return 0;
        }
        case WM_KEYDOWN: {
            if (!controller_) return DefWindowProcW(hwnd_, message, wParam, lParam);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && wParam == 'N') { FocusNewTask(); return 0; }
            if (ctrl && wParam == 'F') {
                if (controller_->State().route == Route::History) historyPage_->FocusSearch();
                else { if (controller_->State().route == Route::Statistics) controller_->Navigate(Route::AllTasks); taskPage_->FocusSearch(); }
                RequestRender();
                return 0;
            }
            if (ctrl && wParam == 'T') { SetTopmost(!controller_->State().topmost); return 0; }
            if (ctrl && wParam == 'H') { controller_->Navigate(Route::History); return 0; }
            const UINT key = static_cast<UINT>(wParam);
            bool handled = false;
            if (confirm_.IsOpen()) handled = confirm_.OnKeyDown(key, ctrl, shift);
            else if (settings_.IsOpen()) handled = settings_.OnKeyDown(key, ctrl, shift);
            else if (taskPage_->Visible()) handled = taskPage_->OnKeyDown(key, ctrl, shift);
            else if (historyPage_->Visible()) handled = historyPage_->OnKeyDown(key, ctrl, shift);
            // Unhandled keys (Tab, F10, arrows when nothing is focused, ...) must reach the default
            // window procedure instead of being swallowed by the frameless window.
            if (!handled) return DefWindowProcW(hwnd_, message, wParam, lParam);
            RequestRender(); return 0;
        }
        case WM_CHAR: {
            // Do not swallow characters when nothing handles them.
            if (auto* text = ActiveTextBox()) {
                if (text->OnChar(static_cast<wchar_t>(wParam))) { RequestRender(); return 0; }
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        case WM_IME_STARTCOMPOSITION: if (auto* text = ActiveTextBox()) { text->UpdateImePosition(hwnd_, dpi_); return text->HandleImeStart(hwnd_) ? 0 : DefWindowProcW(hwnd_, message, wParam, lParam); } break;
        // GCS_RESULTSTR is consumed here and the message is NOT forwarded to DefWindowProcW, so the
        // system does not synthesise WM_IME_CHAR for it; that avoids inserting the result twice.
        // WM_IME_CHAR is still left to DefWindowProcW (default branch) for IMEs that emit it
        // directly without a composition result string, where it becomes a normal WM_CHAR.
        case WM_IME_COMPOSITION: if (auto* text = ActiveTextBox()) { text->HandleImeComposition(hwnd_, lParam); text->UpdateImePosition(hwnd_, dpi_); RequestRender(); return 0; } break;
        case WM_IME_ENDCOMPOSITION: if (auto* text = ActiveTextBox()) { text->HandleImeEnd(hwnd_); RequestRender(); return 0; } break;
        case WM_SETFOCUS: if (auto* text = ActiveTextBox()) { text->OnFocus(true); text->UpdateImePosition(hwnd_, dpi_); } return 0;
        case WM_KILLFOCUS: if (auto* text = ActiveTextBox()) text->OnFocus(false); return 0;
        case WM_APP_DB_COMPLETION: if (controller_) controller_->OnCompletions(); RequestRender(); return 0;
        case WM_APP_TRAY: {
            const auto action = tray_.HandleMessage(wParam, lParam);
            if (action == TrayAction::NewTask) ShowFromTray(true);
            else if (action == TrayAction::Open) ShowFromTray(false);
            else if (action == TrayAction::Exit) { exiting_ = true; SaveWindowConfig(); DestroyWindow(hwnd_); }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // Losing the capture (alt-tab, another window taking it, the system cancelling it) ends
            // the drag: whatever was pressed must not stay latched as the active title button.
            pressedTitle_ = nullptr;
            return 0;
        case WM_CLOSE:
            // Hiding into a tray that was never registered would make the app unreachable.
            ReleaseMouseCapture();
            if (controller_ && trayAvailable_ && controller_->State().closeToTray && !exiting_) ShowWindow(hwnd_, SW_HIDE);
            else { exiting_ = true; SaveWindowConfig(); DestroyWindow(hwnd_); }
            return 0;
        case WM_DESTROY: ReleaseMouseCapture(); CursorFactory::Destroy(); tray_.Remove(); scheduler_.Stop(); KillTimer(hwnd_, 98); KillTimer(hwnd_, 99); KillTimer(hwnd_, kDeviceRetryTimer); deviceRetryArmed_ = false; if (exiting_) PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace todo
