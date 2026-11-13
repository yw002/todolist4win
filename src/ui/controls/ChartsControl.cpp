#include "ui/controls/ChartsControl.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <d2d1_1helper.h>
#include <wrl/client.h>

#include "ui/AnimationClock.h"
#include "ui/Dpi.h"

namespace todo::ui {

void ChartsControl::SetStats(const StatsSnapshot& stats, double nowSeconds) { stats_ = stats; animationStart_ = nowSeconds; }
void ChartsControl::SetTime(double nowSeconds) noexcept { nowSeconds_ = nowSeconds; }

void ChartsControl::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    const float progress = AnimationClock::AnimationsEnabled() ? std::clamp(static_cast<float>((nowSeconds_ - animationStart_) / 0.300), 0.0F, 1.0F) : 1.0F;
    const float width = bounds_.right - bounds_.left;
    const float height = bounds_.bottom - bounds_.top;
    const D2D1_RECT_F ring{bounds_.left, bounds_.top, bounds_.left + width * 0.36F, bounds_.top + height * 0.52F};
    const D2D1_RECT_F bars{bounds_.left + width * 0.38F, bounds_.top, bounds_.right, bounds_.top + height * 0.52F};
    const D2D1_RECT_F trend{bounds_.left, bounds_.top + height * 0.55F, bounds_.right, bounds_.bottom};
    DrawRing(context, resources, ring, progress);
    DrawBars(context, resources, bars, progress);
    DrawTrend(context, resources, trend, progress);
}

void ChartsControl::DrawRing(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress) {
    resources.DrawGlassPanel(area, 14);
    resources.DrawText(L"当前任务完成率", {area.left + 16, area.top + 12, area.right - 16, area.top + 38}, nullptr, true);
    const D2D1_POINT_2F center{(area.left + area.right) / 2, area.top + (area.bottom - area.top) * 0.60F};
    const float radius = std::min(area.right - area.left, area.bottom - area.top) * 0.24F;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    auto trackColor = resources.Colors().muted; trackColor.a = 0.18F;
    context.CreateSolidColorBrush(trackColor, &track);
    context.DrawEllipse({center, radius, radius}, track.Get(), 12.0F);
    const float rate = static_cast<float>(stats_.completionRate) * progress;
    if (rate > 0.0F) {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        ID2D1Factory* factory = nullptr;
        context.GetFactory(&factory);
        if (factory && SUCCEEDED(factory->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            const int segments = std::max(2, static_cast<int>(rate * 96));
            const auto point = [&](int i) { const float angle = -3.14159265F / 2 + rate * 6.2831853F * i / segments; return D2D1_POINT_2F{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius}; };
            sink->BeginFigure(point(0), D2D1_FIGURE_BEGIN_HOLLOW);
            for (int i = 1; i <= segments; ++i) sink->AddLine(point(i));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            context.DrawGeometry(geometry.Get(), resources.AccentBrush(), 12.0F);
        }
        if (factory) factory->Release();
    }
    resources.DrawText(std::to_wstring(static_cast<int>(stats_.completionRate * 100 + 0.5)) + L"%", {center.x - radius, center.y - 20, center.x + radius, center.y + 20}, nullptr, true, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ChartsControl::DrawBars(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress) {
    resources.DrawGlassPanel(area, 14);
    resources.DrawText(L"分类任务与完成量", {area.left + 16, area.top + 12, area.right - 16, area.top + 38}, nullptr, true);
    if (stats_.categories.empty()) { resources.DrawText(L"暂无分类数据", {area.left, area.top + 70, area.right, area.top + 104}, resources.MutedBrush(), false, false, DWRITE_TEXT_ALIGNMENT_CENTER); return; }
    std::int64_t maximum = 1;
    for (const auto& item : stats_.categories) maximum = std::max(maximum, item.total);
    const float rowHeight = std::min(42.0F, (area.bottom - area.top - 54) / static_cast<float>(stats_.categories.size()));
    for (std::size_t i = 0; i < stats_.categories.size() && i < 6; ++i) {
        const auto& item = stats_.categories[i];
        const float y = area.top + 48 + i * rowHeight;
        resources.DrawText(Dpi::Utf8ToWide(item.name), {area.left + 16, y, area.left + 110, y + 22}, nullptr, false, true);
        const float left = area.left + 116;
        const float full = (area.right - left - 48) * static_cast<float>(item.total) / maximum * progress;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> base;
        auto color = resources.Colors().muted; color.a = 0.18F;
        context.CreateSolidColorBrush(color, &base);
        resources.DrawRoundRect({left, y + 4, left + full, y + 16}, 6, base.Get());
        const float completed = (area.right - left - 48) * static_cast<float>(item.completed) / maximum * progress;
        resources.DrawRoundRect({left, y + 4, left + completed, y + 16}, 6, resources.AccentBrush());
        resources.DrawText(std::to_wstring(item.completed) + L"/" + std::to_wstring(item.total), {area.right - 46, y, area.right - 8, y + 22}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void ChartsControl::DrawTrend(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& area, float progress) {
    resources.DrawGlassPanel(area, 14);
    resources.DrawText(L"近 7 天完成事件趋势", {area.left + 16, area.top + 12, area.right - 16, area.top + 38}, nullptr, true);
    if (stats_.sevenDayTrend.empty()) { resources.DrawText(L"暂无完成事件", {area.left, area.top + 60, area.right, area.top + 92}, resources.MutedBrush(), false, false, DWRITE_TEXT_ALIGNMENT_CENTER); return; }
    std::int64_t maximum = 1;
    for (const auto& day : stats_.sevenDayTrend) maximum = std::max(maximum, day.completedEvents);
    const float left = area.left + 34, right = area.right - 24, top = area.top + 52, bottom = area.bottom - 34;
    context.DrawLine({left, bottom}, {right, bottom}, resources.MutedBrush(), 1.0F);
    const float step = stats_.sevenDayTrend.size() > 1 ? (right - left) / (stats_.sevenDayTrend.size() - 1) : 0;
    for (std::size_t i = 0; i < stats_.sevenDayTrend.size(); ++i) {
        const float x = left + step * i;
        const float y = bottom - (bottom - top) * static_cast<float>(stats_.sevenDayTrend[i].completedEvents) / maximum * progress;
        if (i > 0) {
            const float px = left + step * (i - 1);
            const float py = bottom - (bottom - top) * static_cast<float>(stats_.sevenDayTrend[i - 1].completedEvents) / maximum * progress;
            context.DrawLine({px, py}, {x, y}, resources.AccentBrush(), 2.0F);
        }
        context.FillEllipse({{x, y}, 3.5F, 3.5F}, resources.AccentBrush());
        resources.DrawText(std::to_wstring(stats_.sevenDayTrend[i].day.month) + L"/" + std::to_wstring(stats_.sevenDayTrend[i].day.day), {x - 24, bottom + 6, x + 24, bottom + 24}, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

}  // namespace todo::ui
