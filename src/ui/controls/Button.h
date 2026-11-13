#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ui/IconRenderer.h"
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
    // An icon replaces the label but not the stored text: the text stays as the fallback for a render
    // target without SVG support, and as the accessible name.
    void SetIcon(std::optional<IconId> id) noexcept;
    void SetIconSize(float dip) noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseUp(float x, float y) override;
private:
    std::wstring text_;
    std::function<void()> onClick_;
    std::optional<IconId> icon_;
    bool pressed_{false};
    bool accent_{false};
    bool danger_{false};
    float radius_{10.0F};
    float iconSize_{17.0F};
};

}  // namespace todo::ui
