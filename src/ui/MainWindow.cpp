#include "ui/MainWindow.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <memory>
#include <string>
#include <vector>
#include <windowsx.h>

#include "app/Win32UiDispatcher.h"
#include "config/AppPaths.h"
#include "domain/BodyDocument.h"
#include "resources/resource.h"
#include "ui/CursorFactory.h"
#include "ui/AppName.h"
#include "ui/Dpi.h"

namespace todo {
namespace {
constexpr wchar_t kWindowClass[] = L"TodoListWin.MainWindow";
// One-shot retry timer used when the D3D/D2D device could not be rebuilt yet.
constexpr UINT_PTR kDeviceRetryTimer = 97;
// Client coordinate far outside any window, used to clear hover state on every control at once.
constexpr float kNoHover = -1.0e6F;
// Posted once a window transition has reached its final rectangle; the finish step resizes the
// swap chain / hides / minimizes from there instead of doing it inside WM_PAINT.
constexpr UINT kFinishTransitionMessage = WM_APP + 32;
constexpr double kAppearSeconds = 0.36;
constexpr double kCloseSeconds = 0.26;
constexpr double kMinimizeSeconds = 0.26;
constexpr double kMaximizeSeconds = 0.22;
constexpr double kRestoreSeconds = 0.24;
// Fraction of the final window rectangle the entrance/minimize transition starts from. 0.05 makes
// the window collapse to a few dozen pixels - visually "gone" - instead of a visible thumbnail.
constexpr float kTransitionScale = 0.05F;
constexpr wchar_t kDefaultAppName[] = L"TodoList";

// Normalizes the display name coming from the settings card or from app-name.txt. Control
// characters would break the title bar and the tray tooltip, so they are dropped rather than shown.
std::wstring SanitizeAppName(std::wstring value) {
    std::wstring clean;
    clean.reserve(value.size());
    for (wchar_t character : value) {
        if (character < 0x20 || character == 0x7F) continue;
        clean.push_back(character);
    }
    while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'\t')) clean.erase(clean.begin());
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'\t')) clean.pop_back();
    if (clean.size() > kMaxAppNameLength) clean.resize(kMaxAppNameLength);
    return clean.empty() ? std::wstring(kDefaultAppName) : clean;
}
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
    appName_ = LoadAppName();
    maximized_ = config_.window.maximized;
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
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, kWindowClass, appName_.c_str(), style, initial.left, initial.top, initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, instance_, this);
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
    if (trayAvailable_) tray_.SetTip(appName_);
    ApplyRoundedCorners();
    // Deliberately no ShowWindow here: the window must stay hidden until ShowMainWindow has shrunk
    // it to the entrance rectangle. Showing it in Create() was the first-frame flash.
    SyncBuffersToClient();
    UpdateRestingRect();
    Layout();
    // Dialogs own their transitions; they need the window to schedule frames while animating.
    settings_.SetRequestFrame([this] { RequestRender(); });
    confirm_.SetRequestFrame([this] { RequestRender(); });
    return true;
}

void MainWindow::SetController(AppController& controller) {
    controller_ = &controller;
    taskPage_ = std::make_unique<ui::TaskPage>(controller, [this](const Task& task) { OpenTaskEditor(task); }, [this](const Task& task) { RequestDeleteTask(task); });
    historyPage_ = std::make_unique<ui::HistoryPage>(controller, [this](const std::vector<std::int64_t>& ids) { RequestDeleteHistory(ids); });
    statisticsPage_ = std::make_unique<ui::StatisticsPage>(controller);
    navigation_ = std::make_unique<ui::NavigationView>(controller);
    navigation_->SetOnEditCategory([this](std::int64_t id) { OpenCategoryEditor(id); });
    // The label is kept only as a fallback for a render target without SVG support; the icon is what
    // is actually drawn (Button::Paint skips the label once the artwork rendered).
    topmostButton_.SetIcon(ui::IconId::Pin); minimizeButton_.SetIcon(ui::IconId::Minus); maximizeButton_.SetIcon(ui::IconId::Square); closeButton_.SetIcon(ui::IconId::Close);
    themeButton_.SetIcon(ui::IconId::Sun); settingsButton_.SetIcon(ui::IconId::Database); retryButton_.SetIcon(ui::IconId::RotateCw);
    retryButton_.SetText(L"重试"); settingsButton_.SetText(L"设置"); topmostButton_.SetText(L"置顶"); themeButton_.SetText(L"主题");
    minimizeButton_.SetText(L"—"); maximizeButton_.SetText(L"□"); closeButton_.SetText(L"×");
    topmostButton_.SetOnClick([this] { SetTopmost(!controller_->State().topmost); });
    minimizeButton_.SetOnClick([this] { StartMinimize(); });
    maximizeButton_.SetOnClick([this] { ToggleMaximize(); });
    closeButton_.SetOnClick([this] { SendMessageW(hwnd_, WM_CLOSE, 0, 0); });
    themeButton_.SetOnClick([this] { const bool dark = theme_.Palette().dark; controller_->SetThemeMode(dark ? ThemeMode::Light : ThemeMode::Dark); theme_.BeginTransition(controller_->State().themeMode, animationClock_.Now()); animationUntil_ = animationClock_.Now() + 0.35; RequestRender(); });
    settingsButton_.SetOnClick([this] { OpenSettings(); });
    for (auto* button : {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &settingsButton_, &retryButton_}) button->SetRadius(17.0F);
    closeButton_.SetDanger(true);
    SetTimer(hwnd_, 99, 5000, nullptr);
    // Retrying reuses the open worker/factory; no credentials or connection strings are involved.
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
    const bool wasVisible = IsWindowVisible(hwnd_) != FALSE;
    if (!wasVisible) {
        // Put the hidden window at the exact geometry it is going to end up at, then shrink it to the
        // entrance rectangle. Establishing the final client size before the first ShowWindow is what
        // makes the swap chain the right size; previously the window was shown full size first and
        // the WM_SIZE that followed was swallowed by the animation.
        const RECT target = maximized_ ? WorkAreaRect() : RestoreTargetRect();
        if (target.right > target.left && target.bottom > target.top) {
            SetWindowPos(hwnd_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SyncBuffersToClient();
        // Warm the render path while the window is still hidden. The first D2D frame compiles
        // shaders and rasterizes the icon atlas; doing that after ShowWindow blocked the UI thread
        // long enough for the entrance animation to skip to its final rectangle (perceived flash).
        Render();
        StartAppear(target, true);
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    ApplyRoundedCorners();
    SetForegroundWindow(hwnd_);
    RequestRender();
}

void MainWindow::StartAppear(const RECT& finalRect, bool placeBeforeShowing) noexcept {
    rectAnimating_ = false;
    transition_ = Transition::None;
    if (!hwnd_ || finalRect.right <= finalRect.left || finalRect.bottom <= finalRect.top) return;
    if (placeBeforeShowing) {
        SetWindowPos(hwnd_, nullptr, finalRect.left, finalRect.top, finalRect.right - finalRect.left, finalRect.bottom - finalRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!AnimationClock::AnimationsEnabled()) {
        if (renderer_.HasDeviceResources()) { renderer_.SetVisualScale(1.0F); renderer_.SetVisualOpacity(1.0F); }
        SyncBuffersToClient();
        return;
    }
    BeginTransition(Transition::Appear, DockRect(finalRect), finalRect, kAppearSeconds);
    if (rectAnimating_) animationUntil_ = animationClock_.Now() + kAppearSeconds;
}

void MainWindow::StartAppearFromCurrent() noexcept {
    // Restoring from the taskbar leaves the HWND at the tiny rectangle the minimize transition
    // ended on. The swap chain still has the full-size buffers, so the content can grow back without
    // ever exposing a full-size window for one frame.
    if (!hwnd_ || IsIconic(hwnd_)) return;
    RECT current{};
    if (!GetWindowRect(hwnd_, &current)) return;
    const RECT target = maximized_ ? WorkAreaRect() : RestoreTargetRect();
    if (!AnimationClock::AnimationsEnabled()) {
        SetWindowPos(hwnd_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SyncBuffersToClient();
        return;
    }
    const UINT bufferWidth = renderer_.HasDeviceResources() ? renderer_.PixelWidth() : static_cast<UINT>(std::max(1L, target.right - target.left));
    const UINT bufferHeight = renderer_.HasDeviceResources() ? renderer_.PixelHeight() : static_cast<UINT>(std::max(1L, target.bottom - target.top));
    BeginTransition(Transition::Appear, current, target, kAppearSeconds, bufferWidth, bufferHeight);
    if (rectAnimating_) animationUntil_ = animationClock_.Now() + kAppearSeconds;
}

void MainWindow::StartClose(bool hideToTray) noexcept {
    if (!hwnd_) return;
    // A minimized/hidden window has no visible frame to shrink, so those states close immediately.
    if (!AnimationClock::AnimationsEnabled() || closePending_ || IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
        if (hideToTray) ShowWindow(hwnd_, SW_HIDE);
        else { exiting_ = true; SaveWindowConfig(); DestroyWindow(hwnd_); }
        return;
    }
    SyncBuffersToClient();
    RECT current{};
    GetWindowRect(hwnd_, &current);
    closePending_ = true;
    closeToTray_ = hideToTray;
    closeStart_ = animationClock_.Now();
    BeginTransition(Transition::Close, current, DockRect(current), kCloseSeconds);
    if (!rectAnimating_) {
        // The window could not start the animation (e.g. it lost visibility in between): close now
        // rather than leaving the process waiting for frames that will never arrive.
        closePending_ = false;
        if (hideToTray) ShowWindow(hwnd_, SW_HIDE);
        else { exiting_ = true; SaveWindowConfig(); DestroyWindow(hwnd_); }
        return;
    }
    animationUntil_ = closeStart_ + kCloseSeconds;
    RequestRender();
}

void MainWindow::StartMinimize() noexcept {
    if (!hwnd_ || rectAnimating_ || IsIconic(hwnd_)) return;
    if (!AnimationClock::AnimationsEnabled()) { minimizedByAnimation_ = true; ShowWindow(hwnd_, SW_MINIMIZE); return; }
    SyncBuffersToClient();
    RECT current{};
    GetWindowRect(hwnd_, &current);
    BeginTransition(Transition::Minimize, current, DockRect(current), kMinimizeSeconds);
    if (rectAnimating_) animationUntil_ = animationClock_.Now() + kMinimizeSeconds;
}

void MainWindow::ToggleMaximize() noexcept {
    if (!hwnd_ || rectAnimating_) return;
    // Do not clear minimizedByAnimation_ here: WM_SIZE(SIZE_RESTORED) consumes it to start the
    // grow-back animation. Clearing it first made the restored window stay at the minimum size.
    if (IsIconic(hwnd_)) { ShowWindow(hwnd_, SW_RESTORE); return; }
    if (maximized_) StartRestore();
    else StartMaximize();
}

void MainWindow::StartMaximize() noexcept {
    if (!hwnd_ || maximized_ || rectAnimating_) return;
    RECT current{};
    if (!GetWindowRect(hwnd_, &current)) return;
    // The current rectangle is the geometry to return to; remember it before the window grows.
    restingRect_ = current;
    restingRectValid_ = true;
    SyncBuffersToClient();
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT bufferWidth = static_cast<UINT>(std::max(1L, client.right));
    const UINT bufferHeight = static_cast<UINT>(std::max(1L, client.bottom));
    const RECT workArea = WorkAreaRect();
    maximized_ = true;
    ApplyRoundedCorners();
    UpdateTitleIcons();
    if (!AnimationClock::AnimationsEnabled()) {
        SetWindowPos(hwnd_, nullptr, workArea.left, workArea.top, workArea.right - workArea.left, workArea.bottom - workArea.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SyncBuffersToClient();
        RequestRender();
        return;
    }
    BeginTransition(Transition::Maximize, current, workArea, kMaximizeSeconds, bufferWidth, bufferHeight);
    if (rectAnimating_) animationUntil_ = animationClock_.Now() + kMaximizeSeconds;
}

void MainWindow::StartRestore() noexcept {
    if (!hwnd_ || !maximized_ || rectAnimating_) return;
    RECT current{};
    if (!GetWindowRect(hwnd_, &current)) return;
    SyncBuffersToClient();
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT bufferWidth = static_cast<UINT>(std::max(1L, client.right));
    const UINT bufferHeight = static_cast<UINT>(std::max(1L, client.bottom));
    const RECT target = RestoreTargetRect();
    maximized_ = false;
    ApplyRoundedCorners();
    UpdateTitleIcons();
    if (!AnimationClock::AnimationsEnabled()) {
        SetWindowPos(hwnd_, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
        SyncBuffersToClient();
        RequestRender();
        return;
    }
    BeginTransition(Transition::Restore, current, target, kRestoreSeconds, bufferWidth, bufferHeight);
    if (rectAnimating_) animationUntil_ = animationClock_.Now() + kRestoreSeconds;
}

void MainWindow::BeginTransition(Transition transition, const RECT& from, const RECT& to, double seconds, UINT bufferWidth, UINT bufferHeight) noexcept {
    if (!hwnd_ || transition == Transition::None) return;
    rectAnimFrom_ = from;
    rectAnimTo_ = to;
    rectAnimStart_ = animationClock_.Now();
    rectAnimSeconds_ = std::max(0.01, seconds);
    transition_ = transition;
    rectAnimating_ = true;
    if (bufferWidth > 0 && bufferHeight > 0) {
        animFinalClientWidth_ = bufferWidth;
        animFinalClientHeight_ = bufferHeight;
    } else {
        RECT client{};
        GetClientRect(hwnd_, &client);
        animFinalClientWidth_ = static_cast<UINT>(std::max(1L, client.right));
        animFinalClientHeight_ = static_cast<UINT>(std::max(1L, client.bottom));
    }
    animFinalBounds_ = BoundsForClient(animFinalClientWidth_, animFinalClientHeight_);
    SetWindowPos(hwnd_, nullptr, from.left, from.top, from.right - from.left, from.bottom - from.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (renderer_.HasDeviceResources()) {
        UpdateContentScale();
        renderer_.SetVisualOpacity(transition == Transition::Appear || transition == Transition::Restore ? 0.0F : 1.0F);
    }
    Layout();
    RequestRender();
}

void MainWindow::AdvanceWindowRectAnimation(double now) noexcept {
    if (!rectAnimating_ || !hwnd_) return;
    const double linear = std::clamp((now - rectAnimStart_) / rectAnimSeconds_, 0.0, 1.0);
    // Ease-out cubic: quickly leaves the tiny rectangle, then settles into the final rectangle without
    // the overshoot that would scale the swap chain past 100% and clip the window edges.
    const float eased = 1.0F - std::pow(1.0F - static_cast<float>(linear), 3.0F);
    const auto lerp = [eased](LONG from, LONG to) { return from + static_cast<LONG>(std::lround((to - from) * eased)); };
    RECT rect{lerp(rectAnimFrom_.left, rectAnimTo_.left), lerp(rectAnimFrom_.top, rectAnimTo_.top),
              lerp(rectAnimFrom_.right, rectAnimTo_.right), lerp(rectAnimFrom_.bottom, rectAnimTo_.bottom)};
    if (rect.right <= rect.left) rect.right = rect.left + 1;
    if (rect.bottom <= rect.top) rect.bottom = rect.top + 1;
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateContentScale();
    if (renderer_.HasDeviceResources()) {
        float opacity = 1.0F;
        if (transition_ == Transition::Appear || transition_ == Transition::Restore) opacity = eased;
        else if (transition_ == Transition::Close || transition_ == Transition::Minimize) opacity = 1.0F - eased;
        renderer_.SetVisualOpacity(opacity);
    }
    if (linear >= 1.0) {
        rectAnimating_ = false;
        // Finish outside WM_PAINT: closing destroys the HWND and minimizing hands it to the system,
        // neither of which may run on the paint stack.
        PostMessageW(hwnd_, kFinishTransitionMessage, 0, 0);
    }
}

void MainWindow::FinishTransition() {
    const Transition finished = transition_;
    transition_ = Transition::None;
    if (!hwnd_) return;
    if (finished == Transition::Close) { FinishCloseAction(); return; }
    if (finished == Transition::Minimize) {
        // The window is now a tiny rectangle at the bottom edge. Minimizing from there keeps the
        // system's own taskbar animation and never exposes a full-size frame. The rectangle stays
        // tiny on purpose: WM_SIZE(SIZE_RESTORED) grows it back with the same content scale.
        minimizedByAnimation_ = true;
        ShowWindow(hwnd_, SW_MINIMIZE);
        if (renderer_.HasDeviceResources()) { renderer_.SetVisualScale(1.0F); renderer_.SetVisualOpacity(1.0F); }
        return;
    }
    SyncBuffersToClient();
    ApplyRoundedCorners();
    RequestRender();
}

void MainWindow::UpdateContentScale() noexcept {
    if (!renderer_.HasDeviceResources()) return;
    if (!rectAnimating_) { renderer_.SetVisualScale(1.0F); renderer_.SetVisualOpacity(1.0F); return; }
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (animFinalClientWidth_ == 0 || animFinalClientHeight_ == 0) return;
    const float scaleX = static_cast<float>(client.right) / static_cast<float>(animFinalClientWidth_);
    const float scaleY = static_cast<float>(client.bottom) / static_cast<float>(animFinalClientHeight_);
    if (transition_ == Transition::Maximize || transition_ == Transition::Restore) {
        // The window aspect ratio changes when it becomes the work area, so the content has to stretch
        // on both axes; a uniform scale would either clip the bottom or leave empty bands.
        renderer_.SetVisualStretch(scaleX, scaleY);
    } else {
        renderer_.SetVisualScale(scaleX);
    }
}

void MainWindow::SyncBuffersToClient() {
    if (!hwnd_ || !renderer_.HasDeviceResources()) { if (hwnd_) Layout(); return; }
    // A minimized window reports its tiny restore rectangle as the client. Resizing the swap chain to
    // that would throw away the full-size buffers the taskbar grow-back animation needs.
    if (IsIconic(hwnd_) || minimizedByAnimation_) { Layout(); return; }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = static_cast<UINT>(std::max(1L, client.right));
    const UINT height = static_cast<UINT>(std::max(1L, client.bottom));
    // This is the fix for the 14 px acrylic band: the swap chain must always have exactly the client
    // size, including after a transition ends or after ShowWindow has settled a hidden window.
    if (renderer_.PixelWidth() != width || renderer_.PixelHeight() != height) {
        const bool ok = renderer_.ResizeBuffers(width, height, dpi_);
    }
    if (renderer_.VisualScale() != 1.0F) renderer_.SetVisualScale(1.0F);
    if (renderer_.VisualOpacity() != 1.0F) renderer_.SetVisualOpacity(1.0F);
    Layout();
}

void MainWindow::UpdateRestingRect() noexcept {
    // A finished transition can leave the HWND at the tiny dock rectangle for a few milliseconds
    // before the finish message runs (minimize/close). Persisting that rectangle would make the next
    // restore or launch grow to the minimum size instead of the user's real geometry.
    if (!hwnd_ || IsIconic(hwnd_) || maximized_ || rectAnimating_ || transition_ != Transition::None || minimizedByAnimation_) return;
    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect)) return;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    restingRect_ = rect;
    restingRectValid_ = true;
}

void MainWindow::FinishCloseAction() {
    closePending_ = false;
    rectAnimating_ = false;
    transition_ = Transition::None;
    if (!hwnd_) return;
    // The transition ends with the window at the tiny rectangle. Hide first, then restore the resting
    // geometry while nothing can be seen: SaveWindowConfig and the next ShowMainWindow must never
    // observe (or persist) the tiny rectangle.
    ShowWindow(hwnd_, SW_HIDE);
    if (restingRectValid_) {
        SetWindowPos(hwnd_, nullptr, restingRect_.left, restingRect_.top, restingRect_.right - restingRect_.left, restingRect_.bottom - restingRect_.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (renderer_.HasDeviceResources()) { renderer_.SetVisualScale(1.0F); renderer_.SetVisualOpacity(1.0F); }
    SyncBuffersToClient();
    if (closeToTray_) return;
    exiting_ = true;
    SaveWindowConfig();
    DestroyWindow(hwnd_);
}

RECT MainWindow::WorkAreaRect() const noexcept {
    RECT probe{};
    if (hwnd_ && GetWindowRect(hwnd_, &probe)) { /* use the monitor the window is on */ }
    else if (restingRectValid_) probe = restingRect_;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST), &monitor)) return probe;
    return monitor.rcWork;
}

RECT MainWindow::DockRect(const RECT& reference) const noexcept {
    const RECT work = WorkAreaRect();
    const int fullWidth = static_cast<int>(std::max(1L, reference.right - reference.left));
    const int fullHeight = static_cast<int>(std::max(1L, reference.bottom - reference.top));
    const int tinyWidth = std::max(1, static_cast<int>(std::lround(fullWidth * kTransitionScale)));
    const int tinyHeight = std::max(1, static_cast<int>(std::lround(fullHeight * kTransitionScale)));
    const int centerX = (reference.left + reference.right) / 2;
    const int left = std::clamp(centerX - tinyWidth / 2, static_cast<int>(work.left), static_cast<int>(work.right) - tinyWidth);
    const int top = static_cast<int>(work.bottom) - tinyHeight;
    return RECT{left, top, left + tinyWidth, top + tinyHeight};
}

RECT MainWindow::RestoreTargetRect() const noexcept {
    if (restingRectValid_) return ClampToVisibleMonitor(restingRect_);
    const UINT placementDpi = dpi_ != 0 ? dpi_ : 96U;
    const int left = Dpi::ToPixels(static_cast<float>(config_.window.leftDip), placementDpi);
    const int top = Dpi::ToPixels(static_cast<float>(config_.window.topDip), placementDpi);
    const int width = Dpi::ToPixels(static_cast<float>(config_.window.widthDip), placementDpi);
    const int height = Dpi::ToPixels(static_cast<float>(config_.window.heightDip), placementDpi);
    return ClampToVisibleMonitor({left, top, left + width, top + height});
}

D2D1_RECT_F MainWindow::BoundsForClient(UINT width, UINT height) const noexcept {
    return {0, 0, Dpi::ToDips(static_cast<long>(width), dpi_), Dpi::ToDips(static_cast<long>(height), dpi_)};
}

std::wstring MainWindow::LoadAppName() const {
    std::ifstream stream(AppPaths::Default().AppNamePath(), std::ios::binary);
    if (!stream) return std::wstring(kDefaultAppName);
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return SanitizeAppName(Dpi::Utf8ToWide(bytes));
}

void MainWindow::SetAppName(const std::string& utf8Name) {
    appName_ = SanitizeAppName(Dpi::Utf8ToWide(utf8Name));
    // Roaming profile, next to config.ini: the display name is independent of window layout
    // settings, so renaming the app never rewrites the user's saved geometry.
    const std::filesystem::path path = AppPaths::Default().AppNamePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (stream) {
        const std::string bytes = Dpi::WideToUtf8(appName_);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (hwnd_) {
        SetWindowTextW(hwnd_, appName_.c_str());
        tray_.SetTip(appName_);
    }
    RequestRender();
}

void MainWindow::ApplyInitialPlacement() noexcept {
    if (!hwnd_) return;
    const UINT placementDpi = dpi_ != 0 ? dpi_ : 96U;
    const int left = Dpi::ToPixels(static_cast<float>(config_.window.leftDip), placementDpi);
    const int top = Dpi::ToPixels(static_cast<float>(config_.window.topDip), placementDpi);
    const int width = Dpi::ToPixels(static_cast<float>(config_.window.widthDip), placementDpi);
    const int height = Dpi::ToPixels(static_cast<float>(config_.window.heightDip), placementDpi);
    const RECT normal = ClampToVisibleMonitor({left, top, left + width, top + height});
    // The normal rectangle is needed even when the window starts maximized: it is the target of the
    // first restore and the geometry persisted on exit.
    restingRect_ = normal;
    restingRectValid_ = true;
    const RECT target = maximized_ ? WorkAreaRect() : normal;
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
    if (maximized_ || IsZoomed(hwnd_)) { SetWindowRgn(hwnd_, nullptr, TRUE); return; }
    const int radius = Dpi::ToPixels(28.0F, dpi_);
    HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, radius, radius);
    if (region) SetWindowRgn(hwnd_, region, TRUE);
}
void MainWindow::ShowFromTray(bool newTask) { ShowMainWindow(); if (newTask) FocusNewTask(); }
void MainWindow::FocusNewTask() { if (!controller_) return; controller_->Navigate(Route::AllTasks); ShowMainWindow(); if (taskPage_) taskPage_->FocusNew(); }

D2D1_RECT_F MainWindow::WindowBounds() const noexcept {
    // During the window-rect animation the HWND is smaller than the content it presents: the layout
    // stays at the final geometry and the composition visual is scaled instead.
    if (rectAnimating_) return animFinalBounds_;
    RECT client{};
    GetClientRect(hwnd_, &client);
    return {0, 0, Dpi::ToDips(client.right, dpi_), Dpi::ToDips(client.bottom, dpi_)};
}

void MainWindow::Layout() noexcept {
    if (!hwnd_) return;
    const auto bounds = WindowBounds();
    const float width = bounds.right;
    // Title bar: icons instead of words. Each control is a 34 DIP circle so the row reads as one
    // rhythm; previously the widths followed the label length (38 / 64 / 74 / 70) and "置顶" was
    // squeezed into the narrowest one, which is what looked broken. The whole row is pinned to the
    // right margin (16 DIP) as one block: 8 DIP inside the caption group and a 16 DIP separator
    // before the action group, so the icons never float in the middle of a wide title bar.
    retryButton_.SetBounds({width - 310, 12, width - 276, 46});
    settingsButton_.SetBounds({width - 268, 12, width - 234, 46});
    topmostButton_.SetBounds({width - 226, 12, width - 192, 46});
    themeButton_.SetBounds({width - 184, 12, width - 150, 46});
    minimizeButton_.SetBounds({width - 134, 12, width - 100, 46});
    maximizeButton_.SetBounds({width - 92, 12, width - 58, 46});
    closeButton_.SetBounds({width - 50, 12, width - 16, 46});
    if (navigation_) navigation_->SetBounds({16, 62, 208, bounds.bottom - 16});
    const D2D1_RECT_F content{220, 62, width - 16, bounds.bottom - 16};
    if (taskPage_) taskPage_->SetBounds(content);
    if (historyPage_) historyPage_->SetBounds(content);
    if (statisticsPage_) statisticsPage_->SetBounds(content);
}

void MainWindow::UpdateTitleIcons() {
    // The theme button advertises the action, not the state: a dark palette shows a sun (click to go
    // light) and a light palette shows a moon. Recomputed from the live palette on every frame so the
    // icon follows the 300 ms cross-fade without a second hook in the toggle handler.
    themeButton_.SetIcon(theme_.Palette().dark ? ui::IconId::Sun : ui::IconId::Moon);
    maximizeButton_.SetIcon(maximized_ ? ui::IconId::Restore : ui::IconId::Square);
}

void MainWindow::OnStateChanged() {
    if (!controller_) return;
    auto& state = controller_->State();
    const bool writeChanged = state.lastSuccessfulWrite != observedWrite_;
    if (writeChanged) {
        observedWrite_ = state.lastSuccessfulWrite;
        if (settings_.IsOpen() && state.overlay != Overlay::Settings) { settings_.Hide(); state.overlay = Overlay::None; }
    }
    if (!state.toast.empty() && (state.toast != observedToast_ || writeChanged)) {
        observedToast_ = state.toast;
        toast_.Show(Dpi::Utf8ToWide(state.toast), animationClock_.Now());
    }
    if (controller_->FlushPendingSearch(false)) SetTimer(hwnd_, 98, 50, nullptr);
    else KillTimer(hwnd_, 98);
    // Blob bytes arrive asynchronously on the DbWorker. Pushing them whenever state changes keeps
    // the editor in sync without the dialog ever touching the database itself.
    if (settings_.IsOpen() && state.overlay == Overlay::TaskEditor) {
        settings_.SetEditorImages(state.editorImages, state.editorImageError);
    }
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
    // Both overlays own an entrance/exit transition; keep asking for frames while either is moving.
    if (settings_.Tick(now)) animationUntil_ = std::max(animationUntil_, now + 0.05);
    if (confirm_.Tick(now)) animationUntil_ = std::max(animationUntil_, now + 0.05);
    // The entrance/exit is a real window animation: SetWindowPos moves the HWND rectangle from (or
    // into) the bottom edge of the work area while the composition visual scales the content.
    AdvanceWindowRectAnimation(now);
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
        std::wstring status;
        if (controller_->State().loading) status = L"本地 · 加载中";
        else if (controller_->State().connection == ConnectionState::Online) status = L"本地 · 已连接";
        else if (controller_->State().readOnly) status = L"本地 · 只读 · 数据可能不是最新";
        else status = L"本地 · 连接中";
        // A missing tray icon must stay visible: it changes what the close button does.
        if (!trayAvailable_) status += L" · 托盘不可用";
        // The name owns as much of the title strip as its glyphs need. It may grow until the action
        // cluster on the right; the status follows it and is dropped only when a very long name
        // leaves no readable room. A fixed 116 DIP box was what clipped earlier long names.
        const float titleContentRight = std::max(56.0F, bounds.right - 310.0F - 12.0F);
        const float nameWidth = resources.MeasureTextWidth(appName_, 4096.0F, true);
        const float nameRight = std::min(56.0F + nameWidth + 2.0F, titleContentRight);
        const float statusRight = std::max(nameRight, std::min(bounds.right - 350.0F, 820.0F));
        const float statusGap = 16.0F;
        const float statusMinWidth = 110.0F;
        const bool showStatus = nameRight + statusGap + statusMinWidth <= statusRight;
        resources.DrawText(appName_, {56, 10, showStatus ? nameRight : titleContentRight, 48}, nullptr, true);
        if (showStatus) {
            resources.DrawText(status, {nameRight + statusGap, 15, statusRight, 44}, resources.MutedBrush(), false, true);
        }
        topmostButton_.SetAccent(controller_->State().topmost);
        UpdateTitleIcons();
        topmostButton_.Paint(context, resources); minimizeButton_.Paint(context, resources); maximizeButton_.Paint(context, resources); closeButton_.Paint(context, resources); themeButton_.Paint(context, resources); settingsButton_.Paint(context, resources); retryButton_.Paint(context, resources);
        navigation_->Paint(context, resources);
        taskPage_->Paint(context, resources); historyPage_->Paint(context, resources); statisticsPage_->Paint(context, resources);
        if (!controller_->State().errorMessage.empty()) {
            // Keep the message inside the bottom status strip, right-aligned, where nothing else is
            // drawn and it can never cover the editor or the footer summary.
            const D2D1_RECT_F error{bounds.right - 660, bounds.bottom - 56, bounds.right - 34, bounds.bottom - 22};
            resources.DrawText(Dpi::Utf8ToWide(controller_->State().errorMessage), error, resources.DangerBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        toast_.SetBounds(bounds);
        toast_.Paint(context, resources);
        settings_.Paint(context, resources);
        confirm_.Paint(context, resources);
    });
    // Defensive resync: if any WM_SIZE path ever leaves the swap chain smaller than the client (the
    // original 14 px acrylic band), correct it on the next frame instead of leaving the gray edge.
    if (!rectAnimating_ && transition_ == Transition::None && !IsIconic(hwnd_) && !minimizedByAnimation_) {
        RECT client{};
        if (GetClientRect(hwnd_, &client) && renderer_.HasDeviceResources()) {
            const UINT width = static_cast<UINT>(std::max(1L, client.right));
            const UINT height = static_cast<UINT>(std::max(1L, client.bottom));
            if (renderer_.PixelWidth() != width || renderer_.PixelHeight() != height) {
                renderer_.ResizeBuffers(width, height, dpi_);
                Layout();
            }
        }
    }
    if (now < animationUntil_ || toast_.Visible()) scheduler_.RequestFrame();
}
void MainWindow::RequestRender() { if (hwnd_) { InvalidateRect(hwnd_, nullptr, FALSE); scheduler_.RequestFrame(); } }

void MainWindow::OpenTaskEditor(const Task& task) {
    if (controller_->State().readOnly) return;
    controller_->State().overlay = Overlay::TaskEditor;
    settings_.ShowTask(WindowBounds(), task, controller_->State().categories, [this](const EditTaskCommand& command) { controller_->EditTask(command); }, [this] { controller_->State().overlay = Overlay::None; });
    // Only the keys are needed here: the document itself is parsed by the dialog, and the worker must
    // never receive the whole JSON or blocks it does not use.
    std::vector<std::string> keys;
    if (!task.bodyDoc.empty()) {
        auto parsed = BodyDocument::Parse(task.bodyDoc);
        if (parsed.ok()) keys = parsed.value().ImageKeys();
    }
    controller_->LoadBodyImages(task.id, keys);
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
void MainWindow::OpenSettings() {
    controller_->State().overlay = Overlay::Settings;
    // The settings card shows the real SQLite file path and lets the user open its folder. It never
    // touches the database itself; the controller owns all storage access.
    const std::wstring localPath = AppPaths::Default().LocalDatabasePath().wstring();
    settings_.ShowSettings(WindowBounds(), localPath, appName_,
        [this](const std::string& name) { SetAppName(name); },
        [this] { controller_->State().overlay = Overlay::None; });
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
    auto saved = configStore_.SaveUiState(config_);
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
    if ((maximized_ || IsIconic(hwnd_) || rectAnimating_) && restingRectValid_) {
        // A maximized/minimized/transitioning window is not at its normal geometry; persist the
        // remembered normal rectangle instead of the work-area or tiny one.
        rect = restingRect_;
    } else {
        GetWindowRect(hwnd_, &rect);
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    config_ = controller_->Config();
    config_.window.leftDip = Dpi::ToDips(rect.left, dpi_);
    config_.window.topDip = Dpi::ToDips(rect.top, dpi_);
    config_.window.widthDip = Dpi::ToDips(rect.right - rect.left, dpi_);
    config_.window.heightDip = Dpi::ToDips(rect.bottom - rect.top, dpi_);
    config_.window.lastDpi = dpi_;
    config_.window.maximized = maximized_ || IsZoomed(hwnd_) != FALSE;
    (void)configStore_.SaveUiState(config_);
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
    ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &settingsButton_, &retryButton_};
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
    ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &settingsButton_, &retryButton_};
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
            // wParam == TRUE means the client area is being recomputed. Returning 0 with the proposed
            // window rectangle makes the client area exactly the window rectangle; when the window is
            // maximized, pin it to the work area so the first frame can never expose a frame inset.
            if (wParam == TRUE) {
                // Only pin the client to the work area once the maximized window has actually reached
                // it. During the maximize/restore animation the HWND is deliberately smaller than the
                // final rectangle, and pinning here would report a full-size client on every animation
                // frame, which clipped the content instead of scaling it.
                if (maximized_ && !rectAnimating_) {
                    MONITORINFO monitor{sizeof(monitor)};
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                        reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] = monitor.rcWork;
                    }
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            // The dock animation deliberately shrinks the window below the user-resize minimum; the
            // system is only allowed to enforce that minimum outside the animation.
            info->ptMinTrackSize = rectAnimating_ ? POINT{1, 1} : POINT{Dpi::ToPixels(760, dpi_), Dpi::ToPixels(560, dpi_)};
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
            // While the dock animation is running the HWND is not the user's window yet: no resize
            // borders and no caption dragging until it settles.
            if (rectAnimating_) return HTCLIENT;
            if (!maximized_ && !IsZoomed(hwnd_)) {
                const int border = Dpi::ToPixels(6, dpi_);
                const bool left = point.x < border, right = point.x >= client.right - border, top = point.y < border, bottom = point.y >= client.bottom - border;
                if (top && left) return HTTOPLEFT; if (top && right) return HTTOPRIGHT; if (bottom && left) return HTBOTTOMLEFT; if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT; if (right) return HTRIGHT; if (top) return HTTOP; if (bottom) return HTBOTTOM;
            }
            if (point.y < Dpi::ToPixels(54, dpi_) && point.x < client.right - Dpi::ToPixels(320, dpi_)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SIZE:
            // Restoring from the taskbar: the window was minimized from a tiny rectangle and that
            // rectangle is still current, so grow back instead of showing a full-size frame.
            if (wParam == SIZE_RESTORED && minimizedByAnimation_) {
                minimizedByAnimation_ = false;
                StartAppearFromCurrent();
                return 0;
            }
            // During a transition the buffers keep the size the transition started from and the
            // composition visual is scaled; outside it the buffers must always match the client.
            if (wParam != SIZE_MINIMIZED) {
                if (rectAnimating_) {
                    UpdateContentScale();
                } else {
                    SyncBuffersToClient();
                    UpdateRestingRect();
                }
            }
            Layout(); ApplyRoundedCorners(); RequestRender(); return 0;
        case WM_MOVE:
            if (!rectAnimating_ && transition_ == Transition::None && !maximized_) UpdateRestingRect();
            return 0;
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
            // The dock animation presents final-size content scaled down, so client coordinates do
            // not match the layout until it settles. Swallow input instead of mis-hitting controls.
            if (rectAnimating_) return 0;
            SetCapture(hwnd_); SetFocus(hwnd_);
            const float x = DipX(lParam), y = DipY(lParam);
            if (confirm_.IsOpen()) { confirm_.OnMouseDown(x, y, static_cast<UINT>(wParam)); return 0; }
            if (settings_.IsOpen()) { settings_.OnMouseDown(x, y, static_cast<UINT>(wParam)); RequestRender(); return 0; }
            ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &settingsButton_, &retryButton_};
            for (auto* button : titleButtons) if (button->HitTest(x, y)) { pressedTitle_ = button; button->OnMouseDown(x, y, static_cast<UINT>(wParam)); RequestRender(); return 0; }
            if (navigation_->HitTest(x, y)) navigation_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else if (taskPage_->Visible()) taskPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else if (historyPage_->Visible()) historyPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            else statisticsPage_->OnMouseDown(x, y, static_cast<UINT>(wParam));
            RequestRender(); return 0;
        }
        case WM_MOUSEMOVE: {
            if (rectAnimating_) return 0;
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
                ui::Control* const titleButtons[] = {&topmostButton_, &minimizeButton_, &maximizeButton_, &closeButton_, &themeButton_, &settingsButton_, &retryButton_};
                for (auto* button : titleButtons) button->OnMouseMove(x, y);
                navigation_->OnMouseMove(x, y); taskPage_->OnMouseMove(x, y); historyPage_->OnMouseMove(x, y);
            }
            RequestRender(); return 0;
        }
        case WM_LBUTTONUP: {
            if (rectAnimating_) { pressedTitle_ = nullptr; ReleaseCapture(); return 0; }
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
        case WM_SYSCOMMAND: {
            const UINT command = wParam & 0xFFF0U;
            // Maximize/restore/minimize are handled here so every path (title button, system menu,
            // taskbar menu, snap maximize) goes through the same animation and icon state.
            if (command == SC_MAXIMIZE) { StartMaximize(); return 0; }
            if (command == SC_MINIMIZE) { StartMinimize(); return 0; }
            if (command == SC_RESTORE) { ToggleMaximize(); return 0; }
            break;
        }
        case WM_NCLBUTTONDBLCLK:
            if (static_cast<UINT>(wParam) == HTCAPTION) { ToggleMaximize(); return 0; }
            break;
        case WM_LBUTTONDBLCLK: { if (rectAnimating_) return 0; const float x = DipX(lParam), y = DipY(lParam); if (!settings_.IsOpen() && navigation_->HitTest(x, y)) navigation_->OnDoubleClick(x, y); else if (!settings_.IsOpen() && taskPage_->Visible()) taskPage_->OnDoubleClick(x, y); RequestRender(); return 0; }
        case WM_MOUSEWHEEL: {
            if (rectAnimating_) return 0;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(hwnd_, &point);
            const float x = Dpi::ToDips(point.x, dpi_), y = Dpi::ToDips(point.y, dpi_);
            if (settings_.IsOpen()) settings_.OnMouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam));
            else if (taskPage_->Visible()) taskPage_->OnMouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam));
            else if (historyPage_->Visible()) historyPage_->OnMouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam));
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
            else if (action == TrayAction::Exit) StartClose(false);
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
            if (closePending_) return 0;
            if (controller_ && trayAvailable_ && controller_->State().closeToTray && !exiting_) StartClose(true);
            else StartClose(false);
            return 0;
        case kFinishTransitionMessage: FinishTransition(); return 0;
        case WM_DESTROY: ReleaseMouseCapture(); CursorFactory::Destroy(); tray_.Remove(); scheduler_.Stop(); KillTimer(hwnd_, 98); KillTimer(hwnd_, 99); KillTimer(hwnd_, kDeviceRetryTimer); deviceRetryArmed_ = false; if (exiting_) PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace todo