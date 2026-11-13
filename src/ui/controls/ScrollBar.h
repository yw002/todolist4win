#pragma once

#include <functional>

#include "ui/controls/Control.h"

namespace todo::ui {

class ScrollBar final : public Control {
public:
    void SetRange(int maximum, int viewport) noexcept;
    void SetOffset(int offset) noexcept;
    void SetOnScroll(std::function<void(int)> callback);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
private:
    void ScrollTo(float y);
    int maximum_{0};
    int viewport_{0};
    int offset_{0};
    bool dragging_{false};
    std::function<void(int)> onScroll_;
};

}  // namespace todo::ui
