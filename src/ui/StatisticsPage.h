#pragma once

#include "app/AppController.h"
#include "ui/controls/Button.h"
#include "ui/controls/ChartsControl.h"

namespace todo::ui {

class StatisticsPage final : public Control {
public:
    explicit StatisticsPage(AppController& controller);
    void SetTime(double nowSeconds) noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseUp(float x, float y) override;
private:
    AppController& controller_;
    ChartsControl charts_;
    Button refresh_;
    double nowSeconds_{0.0};
    std::uint64_t statsVersion_{static_cast<std::uint64_t>(-1)};
};

}  // namespace todo::ui
