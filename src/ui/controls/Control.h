#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include "ui/RenderResources.h"

namespace todo::ui {

class Control {
public:
    virtual ~Control() = default;
    virtual void Paint(ID2D1DeviceContext& context, RenderResources& resources);
    virtual bool HitTest(float x, float y) const noexcept;
    virtual bool OnMouseDown(float x, float y, UINT keyFlags);
    virtual bool OnMouseUp(float x, float y);
    virtual bool OnMouseMove(float x, float y);
    virtual bool OnDoubleClick(float x, float y);
    virtual bool OnMouseWheel(float x, float y, int delta);
    virtual bool OnKeyDown(UINT key, bool ctrl, bool shift);
    virtual bool OnChar(wchar_t value);
    virtual void OnFocus(bool focused);
    void SetBounds(const D2D1_RECT_F& bounds) noexcept;
    void SetVisible(bool visible) noexcept;
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] const D2D1_RECT_F& Bounds() const noexcept;
    [[nodiscard]] bool Visible() const noexcept;
    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] bool Focused() const noexcept;
protected:
    D2D1_RECT_F bounds_{0, 0, 0, 0};
    bool visible_{true};
    bool enabled_{true};
    bool focused_{false};
    bool hovered_{false};
};

}  // namespace todo::ui
