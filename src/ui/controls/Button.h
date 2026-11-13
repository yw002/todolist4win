#pragma once

#include <functional>
#include <string>

#include "ui/controls/Control.h"

namespace todo::ui {

class Button final : public Control {
public:
    Button();
    void SetText(std::wstring text);
    void SetOnClick(std::function<void()> callback);
    void SetAccent(bool accent) noexcept;
    void SetDanger(bool danger) noexcept;
    void SetRadius(float radius) noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseUp(float x, float y) override;
private:
    std::wstring text_;
    std::function<void()> onClick_;
    bool pressed_{false};
    bool accent_{false};
    bool danger_{false};
    float radius_{10.0F};
};

}  // namespace todo::ui
