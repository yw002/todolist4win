#pragma once

#include <windows.h>

namespace todo {

class RenderScheduler {
public:
    explicit RenderScheduler(HWND hwnd = nullptr);
    void Attach(HWND hwnd);
    void RequestFrame() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool OnTimer(UINT_PTR id) noexcept;
private:
    HWND hwnd_{nullptr};
    bool scheduled_{false};
    static constexpr UINT_PTR kTimerId = 41;
};

}  // namespace todo
