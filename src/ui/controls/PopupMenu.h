#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/controls/Control.h"

namespace todo::ui {

class PopupMenu final : public Control {
public:
    struct Item { std::int64_t id{0}; std::wstring text; bool enabled{true}; };
    // Overlays must start hidden. Control::visible_ defaults to true, so without this IsOpen()
    // would answer true before the first Show(), letting an empty dropdown report itself as open.
    PopupMenu() noexcept { visible_ = false; }
    void Show(float x, float y, float width, std::vector<Item> items, std::function<void(std::int64_t)> callback);
    void Hide() noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    [[nodiscard]] bool IsOpen() const noexcept;
private:
    std::vector<Item> items_;
    std::function<void(std::int64_t)> onSelect_;
    int hoveredIndex_{-1};
};

}  // namespace todo::ui
