#pragma once

#include <functional>

#include "ui/controls/Control.h"

namespace todo::ui {

class CheckBox final : public Control {
public:
    void SetChecked(bool checked, double nowSeconds = 0.0) noexcept;
    void SetOnToggle(std::function<void(bool)> callback);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseUp(float x, float y) override;
    [[nodiscard]] bool Checked() const noexcept;
private:
    bool checked_{false};
    double animationStart_{0.0};
    std::function<void(bool)> onToggle_;
};

}  // namespace todo::ui
