#pragma once

#include <functional>
#include <string>

#include "ui/controls/Control.h"

namespace todo::ui {

// Self-drawn month calendar used by the "new task" row to pick a due date.
//
// The popup owns no HWND: it is painted into the already-composited frame and only reports hits
// through HitTest(), exactly like PopupMenu. Every brush used while painting is created inside
// Paint() from the live ID2D1DeviceContext, because a cached brush would outlive a device-lost
// cycle and then draw with a dangling device reference.
class DatePicker final : public Control {
public:
    // Overlays must start hidden. Control::visible_ defaults to true, so without this IsOpen()
    // would answer true before the first Show() and TaskPage::OnMouseDown would swallow every
    // click in the window (this exact bug already shipped once in this code base).
    DatePicker() noexcept { visible_ = false; }

    // Anchors the popup at (x, y) - the caller passes the anchor control's bottom-left corner.
    // currentDate is "YYYY-MM-DD"; an empty (or unparsable) value opens on the current month with
    // no selection.
    void Show(float x, float y, const std::wstring& currentDate);
    void Hide() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;
    // Receives the picked date as "YYYY-MM-DD"; the "清空" button reports an empty string.
    void SetOnPick(std::function<void(const std::wstring&)> callback);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;

private:
    enum class Part { None, PreviousMonth, NextMonth, Today, Clear };
    struct Civil { int year{1970}; int month{1}; int day{1}; };

    void ShiftView(int months) noexcept;
    void Pick(int year, int month, int day);
    [[nodiscard]] int CellCountInView() const noexcept;
    [[nodiscard]] int FirstWeekdayOfView() const noexcept;
    // Top of the day grid for the current month, in bounds-relative DIP. A month spanning five weeks
    // gets its grid centred in the fixed-height popup instead of leaving a conspicuous empty row
    // sitting above the footer.
    [[nodiscard]] float GridTop() const noexcept;
    [[nodiscard]] int HitCellIndex(float x, float y) const noexcept;
    [[nodiscard]] Part HitPart(float x, float y) const noexcept;
    [[nodiscard]] D2D1_RECT_F CellRect(int row, int column) const noexcept;
    [[nodiscard]] D2D1_RECT_F PreviousRect() const noexcept;
    [[nodiscard]] D2D1_RECT_F NextRect() const noexcept;
    [[nodiscard]] D2D1_RECT_F TodayRect() const noexcept;
    [[nodiscard]] D2D1_RECT_F ClearRect() const noexcept;
    void PaintHeader(ID2D1DeviceContext& context, RenderResources& resources);
    void PaintWeekdays(ID2D1DeviceContext& context, RenderResources& resources);
    void PaintGrid(ID2D1DeviceContext& context, RenderResources& resources);
    void PaintFooter(ID2D1DeviceContext& context, RenderResources& resources);
    void PaintNavButton(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& rect, const wchar_t* glyph, Part part, bool large);

    std::function<void(const std::wstring&)> onPick_;
    Civil view_{};
    Civil selected_{};
    Civil today_{};
    bool hasSelection_{false};
    int hoveredIndex_{-1};
    Part hoveredPart_{Part::None};
};

}  // namespace todo::ui
