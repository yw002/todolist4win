#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include <shellapi.h>

namespace todo {

inline constexpr UINT WM_APP_TRAY = WM_APP + 23;

enum class TrayAction { None, NewTask, Open, Exit };

class TrayService {
public:
    ~TrayService();
    bool Create(HWND hwnd, HICON icon);
    // Keeps the tooltip in sync with the user-chosen application name.
    void SetTip(const std::wstring& tip);
    void Remove() noexcept;
    TrayAction HandleMessage(WPARAM wParam, LPARAM lParam);
private:
    HWND hwnd_{nullptr};
    NOTIFYICONDATAW data_{};
    bool added_{false};
};

}  // namespace todo
