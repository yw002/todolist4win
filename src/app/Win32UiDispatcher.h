#pragma once

#include <windows.h>

#include <atomic>

#include "app/UiDispatcher.h"

namespace todo {

inline constexpr UINT WM_APP_DB_COMPLETION = WM_APP + 17;

class Win32UiDispatcher final : public IUiDispatcher {
public:
    void Attach(HWND hwnd) noexcept;
    void Detach() noexcept;
    void PostCompletion() override;
private:
    std::atomic<HWND> hwnd_{nullptr};
};

}  // namespace todo
