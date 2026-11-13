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
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Layout() noexcept;
    void UpdateTitleIcons();
    void Render();
    void RequestRender();
    void ShowFromTray(bool newTask);
    void OpenTaskEditor(const Task& task);
    void OpenCategoryEditor(std::int64_t id);
    void RequestDeleteTask(const Task& task);
    void RequestDeleteHistory(const std::vector<std::int64_t>& ids);
    void OpenConnectionSettings();
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
    // The data source the open settings dialog asked for. While it is set, OnStateChanged() is
    // waiting for the switch to finish: it closes the dialog when the switch was committed and shows
    // the failure inside the card when it was not. The committed source is read from the controller
    // (Config().dataSource only changes on a successful commit), so no extra state has to exist in
    // AppState for this.
    std::optional<DataSourceKind> switchTarget_;
    bool trayAvailable_{false};
    bool deviceRetryArmed_{false};
    std::uint64_t observedWrite_{0};
    std::string observedToast_;
    double animationUntil_{0.0};
    double nextDeviceRecovery_{0.0};
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
    ui::Button connectionButton_;
    ui::Button retryButton_;
    ui::Control* pressedTitle_{nullptr};
    bool maximized_{false};
    // Set while a TME_LEAVE registration is outstanding, so WM_MOUSELEAVE is re-armed exactly once
    // per pointer entry instead of on every mouse move.
    bool mouseTracking_{false};
};

}  // namespace todo
