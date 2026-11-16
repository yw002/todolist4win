#include "app/TrayService.h"

namespace todo {
namespace {
constexpr UINT kTrayId = 7;
constexpr UINT kMenuNew = 1001;
constexpr UINT kMenuOpen = 1002;
constexpr UINT kMenuExit = 1003;
constexpr GUID kTrayGuid = {0xD8183F77, 0xD06A, 0x4E28, {0x9F, 0x12, 0xD7, 0x81, 0x2A, 0xBD, 0x45, 0x01}};
}

TrayService::~TrayService() { Remove(); }
bool TrayService::Create(HWND hwnd, HICON icon) {
    hwnd_ = hwnd;
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd;
    data_.uID = kTrayId;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    data_.uCallbackMessage = WM_APP_TRAY;
    data_.hIcon = icon;
    data_.guidItem = kTrayGuid;
    wcscpy_s(data_.szTip, L"TodoList for Windows");
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) { data_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data_); }
    return added_;
}
void TrayService::SetTip(const std::wstring& tip) {
    if (!added_) return;
    wcsncpy_s(data_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}
void TrayService::Remove() noexcept { if (added_) Shell_NotifyIconW(NIM_DELETE, &data_); added_ = false; hwnd_ = nullptr; }
TrayAction TrayService::HandleMessage(WPARAM, LPARAM lParam) {
    const UINT event = LOWORD(lParam);
    if (event == WM_LBUTTONUP || event == NIN_KEYSELECT) return TrayAction::Open;
    if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU) return TrayAction::None;
    HMENU menu = CreatePopupMenu();
    if (!menu) return TrayAction::None;
    AppendMenuW(menu, MF_STRING, kMenuNew, L"新建任务\tCtrl+N");
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开主窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (command == kMenuNew) return TrayAction::NewTask;
    if (command == kMenuOpen) return TrayAction::Open;
    if (command == kMenuExit) return TrayAction::Exit;
    return TrayAction::None;
}

}  // namespace todo
