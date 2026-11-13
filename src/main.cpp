#include <windows.h>

#include <exception>

#include "app/Application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 1;
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
    return result;
}
