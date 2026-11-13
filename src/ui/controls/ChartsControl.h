#pragma once

#include "domain/Models.h"
#include "ui/controls/Control.h"

namespace todo::ui {

class ChartsControl final : public Control {
public:
    void SetStats(const StatsSnapshot& stats, double nowSeconds);
    void SetTime(double nowSeconds) noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
private:
    void DrawRing(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress);
    void DrawBars(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress);
    void DrawTrend(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress);
    StatsSnapshot stats_;
    double animationStart_{0.0};
    double nowSeconds_{0.0};
};

}  // namespace todo::ui
