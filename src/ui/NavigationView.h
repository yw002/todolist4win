#pragma once

#include <functional>

#include "app/AppController.h"
#include "ui/controls/Control.h"

namespace todo::ui {

class NavigationView final : public Control {
public:
    explicit NavigationView(AppController& controller);
    void SetOnEditCategory(std::function<void(std::int64_t)> callback);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnDoubleClick(float x, float y) override;
    // Drops the hover highlight (used when the pointer leaves the window). Returns true when a
    // highlight was actually cleared, so the caller knows a repaint is needed.
    bool ClearHover() noexcept;
    // Hover classification of the row under (x, y); -1 when no row is hovered.
    [[nodiscard]] int HoverIndex() const noexcept;
private:
    // Row identifiers for hoverIndex_: the three routes come first, then a block for the category
    // rows, then the fixed rows at the bottom of the panel.
    static constexpr int kTopRouteRows = 3;
    static constexpr int kCategoryRowBase = 100;
    static constexpr int kHistoryRow = 200;
    static constexpr int kStatisticsRow = 201;
    static constexpr int kAddCategoryRow = 202;
    [[nodiscard]] int RowAt(float y) const noexcept;
    std::int64_t CategoryAt(float y) const;
    AppController& controller_;
    std::function<void(std::int64_t)> editCategory_;
    int hoverIndex_{-1};
};

}  // namespace todo::ui
