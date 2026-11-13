#pragma once

#include <functional>
#include <string>

#include "ui/controls/Button.h"

namespace todo::ui {

class ModalDialog final : public Control {
public:
    // Overlays must start hidden. Control::visible_ defaults to true, so without this IsOpen()
    // would answer true before the first Show(), Paint would draw an empty confirm card and
    // MainWindow's WM_LBUTTONDOWN would swallow every click behind it.
    ModalDialog() noexcept { visible_ = false; }
    void Show(const D2D1_RECT_F& window, std::wstring title, std::wstring message, std::function<void(bool)> callback);
    void Hide() noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseUp(float x, float y) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    [[nodiscard]] bool IsOpen() const noexcept;
    // Cursor support: true when a dialog button is under the given DIP position.
    [[nodiscard]] bool HitButton(float x, float y) const noexcept;
private:
    std::wstring title_;
    std::wstring message_;
    Button confirm_;
    Button cancel_;
    std::function<void(bool)> callback_;
    bool pressedButton_{false};
};

}  // namespace todo::ui
