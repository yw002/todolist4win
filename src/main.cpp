#include <windows.h>

#include <exception>

#include "app/Application.h"

namespace {

// One instance per interactive session. Without this guard a second copy of the app starts with its
// own in-memory AppConfig; when the older copy exits it rewrites the whole config.ini from that
// stale snapshot and silently rolls back a data-source switch made in the newer copy.
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\TodoListWin.SingleInstance.v1";
constexpr wchar_t kMainWindowClass[] = L"TodoListWin.MainWindow";

HWND FindRunningWindow() noexcept {
    HWND hwnd = nullptr;
    // The mutex may be created a moment before the first process registers and shows its window.
    for (int attempt = 0; attempt < 30 && !hwnd; ++attempt) {
        hwnd = FindWindowW(kMainWindowClass, nullptr);
        if (!hwnd) Sleep(100);
    }
    return hwnd;
}

void ActivateRunningWindow(HWND hwnd) noexcept {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_SHOW);  // brings back a window hidden to the tray
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    const bool alreadyRunning = mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        if (HWND existing = FindRunningWindow()) ActivateRunningWindow(existing);
        CloseHandle(mutex);
        return 0;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        return 1;
    }
    int result = 1;
    try {
        todo::Application application(instance);
        result = application.Run();
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"应用遇到无法恢复的内部错误，请查看日志后重试。", L"TodoList", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"应用遇到未知错误。", L"TodoList", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return result;
}
