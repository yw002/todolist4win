#pragma once

#include <string>

#include "ui/controls/Control.h"

namespace todo::ui {

class ToastView final : public Control {
public:
    // Same overlay rule as the dialogs: start hidden instead of inheriting Control's true.
    // Paint() already bails out on an empty text, but MainWindow::Render() sets the toast bounds to
    // the whole window every frame, so a visible_ that starts true would also make HitTest() claim
    // the entire window until the first SetTime() expiry.
    ToastView() noexcept { visible_ = false; }
    void Show(std::wstring text, double nowSeconds);
    void SetTime(double nowSeconds) noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
private:
    std::wstring text_;
    double shownAt_{0.0};
    double nowSeconds_{0.0};
};

}  // namespace todo::ui
