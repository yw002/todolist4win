#pragma once

#include <windows.h>

#include <functional>

#include <shellapi.h>

namespace todo {

inline constexpr UINT WM_APP_TRAY = WM_APP + 23;

enum class TrayAction { None, NewTask, Open, Exit };

class TrayService {
public:
    ~TrayService();
    bool Create(HWND hwnd, HICON icon);
    void Remove() noexcept;
    TrayAction HandleMessage(WPARAM wParam, LPARAM lParam);
private:
    HWND hwnd_{nullptr};
    NOTIFYICONDATAW data_{};
    bool added_{false};
};

}  // namespace todo
