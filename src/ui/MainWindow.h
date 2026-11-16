#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "app/AppController.h"
#include "app/TrayService.h"
#include "config/ConfigStore.h"
#include "ui/AnimationClock.h"
#include "ui/Backdrop.h"
#include "ui/HistoryPage.h"
#include "ui/NavigationView.h"
#include "ui/RenderEngine.h"
#include "ui/RenderScheduler.h"
#include "ui/SettingsDialog.h"
#include "ui/StatisticsPage.h"
#include "ui/TaskPage.h"
#include "ui/Theme.h"
#include "ui/ToastView.h"
#include "ui/controls/ModalDialog.h"

namespace todo {

class MainWindow {
public:
    MainWindow(HINSTANCE instance, ConfigStore& configStore, AppConfig config);
    ~MainWindow();
    bool Create();
    void SetController(AppController& controller);
    void ShowMainWindow();
    void FocusNewTask();
    void OnStateChanged();
    [[nodiscard]] HWND Handle() const noexcept;

private:
    enum class Transition { None, Appear, Close, Minimize, Maximize, Restore };
    static constexpr double kTransitionSeconds = 0.30;
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Layout() noexcept;
    void UpdateTitleIcons();
    void Render();
    void RequestRender();
    void ShowFromTray(bool newTask);
    // Window transitions. Every entry point ends in the same Advance/Finish pair, so appearance,
    // close, minimize, maximize and restore share one timing model and one content-scale rule.
    void StartAppear(const RECT& finalRect, bool placeBeforeShowing) noexcept;
    void StartAppearFromCurrent() noexcept;
    void StartClose(bool hideToTray) noexcept;
    void StartMinimize() noexcept;
    void ToggleMaximize() noexcept;
    void StartMaximize() noexcept;
    void StartRestore() noexcept;
    void FinishCloseAction();
    void BeginTransition(Transition transition, const RECT& from, const RECT& to, double seconds, UINT bufferWidth = 0, UINT bufferHeight = 0) noexcept;
    void AdvanceWindowRectAnimation(double now) noexcept;
    void FinishTransition();
    void UpdateContentScale() noexcept;
    void SyncBuffersToClient();
    void UpdateRestingRect() noexcept;
    [[nodiscard]] RECT WorkAreaRect() const noexcept;
    [[nodiscard]] RECT DockRect(const RECT& reference) const noexcept;
    [[nodiscard]] RECT RestoreTargetRect() const noexcept;
    [[nodiscard]] D2D1_RECT_F BoundsForClient(UINT width, UINT height) const noexcept;
    // Display name persisted in the user profile (app-name.txt), independently of config.ini.
    [[nodiscard]] std::wstring LoadAppName() const;
    void SetAppName(const std::string& utf8Name);
    void OpenTaskEditor(const Task& task);
    void OpenCategoryEditor(std::int64_t id);
    void RequestDeleteTask(const Task& task);
    void RequestDeleteHistory(const std::vector<std::int64_t>& ids);
    void OpenSettings();
    void SaveWindowConfig();
    void SetTopmost(bool value);
    bool ApplyTopmostToWindow(bool value) noexcept;
    bool RecoverDeviceResources();
    void ApplyRoundedCorners() noexcept;
    void ApplyInitialPlacement() noexcept;
    void ReleaseMouseCapture() noexcept;
    RECT ClampToVisibleMonitor(const RECT& rect) const noexcept;
    D2D1_RECT_F WindowBounds() const noexcept;
    ui::TextBox* ActiveTextBox() noexcept;
    ui::Control* ActivePageFocus() noexcept;
    float DipX(LPARAM lParam) const noexcept;
    float DipY(LPARAM lParam) const noexcept;
    // Cursor + hover plumbing.
    [[nodiscard]] HCURSOR ResolveCursor() noexcept;
    void ClearHoverState() noexcept;

    HINSTANCE instance_{nullptr};
    HWND hwnd_{nullptr};
    ConfigStore& configStore_;
    AppConfig config_;
    AppController* controller_{nullptr};
    UINT dpi_{96};
    bool exiting_{false};
    bool trayAvailable_{false};
    bool deviceRetryArmed_{false};
    std::uint64_t observedWrite_{0};
    std::string observedToast_;
    double animationUntil_{0.0};
    double nextDeviceRecovery_{0.0};
    // Window-rectangle animation state. `restingRect_` is the size/position the window returns to
    // after an entrance or exit, so hiding to the tray never saves the shrunken dock rectangle.
    std::wstring appName_{L"TodoList"};
    RECT restingRect_{};
    bool restingRectValid_{false};
    bool rectAnimating_{false};
    Transition transition_{Transition::None};
    double rectAnimStart_{0.0};
    double rectAnimSeconds_{kTransitionSeconds};
    RECT rectAnimFrom_{};
    RECT rectAnimTo_{};
    UINT animFinalClientWidth_{1};
    UINT animFinalClientHeight_{1};
    D2D1_RECT_F animFinalBounds_{0, 0, 0, 0};
    bool closePending_{false};
    bool closeToTray_{false};
    double closeStart_{0.0};
    RenderEngine renderer_;
    RenderScheduler scheduler_;
    AnimationClock animationClock_;
    Backdrop backdrop_;
    Theme theme_;
    TrayService tray_;
    std::unique_ptr<ui::NavigationView> navigation_;
    std::unique_ptr<ui::TaskPage> taskPage_;
    std::unique_ptr<ui::HistoryPage> historyPage_;
    std::unique_ptr<ui::StatisticsPage> statisticsPage_;
    ui::SettingsDialog settings_;
    ui::ModalDialog confirm_;
    ui::ToastView toast_;
    ui::Button topmostButton_;
    ui::Button minimizeButton_;
    ui::Button maximizeButton_;
    ui::Button closeButton_;
    ui::Button themeButton_;
    ui::Button settingsButton_;
    ui::Button retryButton_;
    ui::Control* pressedTitle_{nullptr};
    bool maximized_{false};
    // Set when the custom minimize transition handed the window to the system; the next
    // WM_SIZE(SIZE_RESTORED) uses it to grow back from the tiny rect instead of flashing full size.
    bool minimizedByAnimation_{false};
    // Set while a TME_LEAVE registration is outstanding, so WM_MOUSELEAVE is re-armed exactly once
    // per pointer entry instead of on every mouse move.
    bool mouseTracking_{false};
};

}  // namespace todo
