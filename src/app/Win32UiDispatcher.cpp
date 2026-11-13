#include "app/Win32UiDispatcher.h"

namespace todo {
void Win32UiDispatcher::Attach(HWND hwnd) noexcept { hwnd_.store(hwnd); }
void Win32UiDispatcher::Detach() noexcept { hwnd_.store(nullptr); }
void Win32UiDispatcher::PostCompletion() { const HWND hwnd = hwnd_.load(); if (hwnd) PostMessageW(hwnd, WM_APP_DB_COMPLETION, 0, 0); }
}
