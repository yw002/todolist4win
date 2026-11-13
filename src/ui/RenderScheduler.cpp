#include "ui/RenderScheduler.h"

namespace todo {

RenderScheduler::RenderScheduler(HWND hwnd) : hwnd_(hwnd) {}
void RenderScheduler::Attach(HWND hwnd) { hwnd_ = hwnd; }
void RenderScheduler::RequestFrame() noexcept {
    if (!hwnd_ || scheduled_) return;
    scheduled_ = SetTimer(hwnd_, kTimerId, 8, nullptr) != 0;
}
void RenderScheduler::Stop() noexcept {
    if (hwnd_ && scheduled_) KillTimer(hwnd_, kTimerId);
    scheduled_ = false;
}
bool RenderScheduler::OnTimer(UINT_PTR id) noexcept {
    if (id != kTimerId) return false;
    Stop();
    return true;
}

}  // namespace todo
