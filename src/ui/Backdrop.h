#pragma once

#include <windows.h>

namespace todo {

class Backdrop {
public:
    bool Enable(HWND hwnd) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool SystemBackdropActive() const noexcept;
private:
    bool active_{false};
    HWND hwnd_{nullptr};
};

}  // namespace todo
