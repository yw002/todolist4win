#include "ui/controls/DatePicker.h"

#include <windows.h>

#include <cwchar>
#include <functional>
#include <string>
#include <utility>

#include <d2d1_1helper.h>
#include <wrl/client.h>

#include "domain/TimeRanges.h"
#include "ui/Dpi.h"

namespace todo::ui {

namespace {

// ---------------------------------------------------------------------------------------------
// Fixed geometry (DIP). The popup is deliberately not resizable: a month grid only needs one
// size, and hard-coding it keeps the hit-testing math identical to the painting math.
// ---------------------------------------------------------------------------------------------
constexpr float kWidth = 280.0F;
constexpr float kHeight = 336.0F;

constexpr float kPadding = 8.0F;          // inner horizontal padding, grid spans left+8 .. right-8
constexpr float kHeaderTop = 8.0F;        // month title row
constexpr float kHeaderHeight = 30.0F;
constexpr float kArrowWidth = 30.0F;
constexpr float kWeekdayTop = 44.0F;      // 日 一 二 ... row
constexpr float kWeekdayHeight = 20.0F;
constexpr float kGridTop = 66.0F;         // 6x7 day grid
constexpr float kCellHeight = 36.0F;
constexpr int kRows = 6;
constexpr int kColumns = 7;
constexpr float kFooterTop = 288.0F;      // 今天 / 清空 row (grid ends at 282)
constexpr float kFooterHeight = 32.0F;
constexpr float kFooterButtonWidth = 72.0F;
constexpr int kCellCount = kRows * kColumns;

// (280 - 2 * 8) / 7 = 37.714285...
float CellWidth() noexcept { return (kWidth - 2.0F * kPadding) / static_cast<float>(kColumns); }

D2D1_ROUNDED_RECT Rounded(const D2D1_RECT_F& rect, float radius) noexcept {
    return D2D1_ROUNDED_RECT{rect, radius, radius};
}

bool Contains(const D2D1_RECT_F& rect, float x, float y) noexcept {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

// Hover tint for the popup's own controls. PanelBrush() is white at roughly 0.30 alpha, which is
// invisible on top of the near-opaque floating surface (ThemePalette::popup), so hover has to be
// accent tinted to register at all.
void FillHover(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& rect, float radius) {
    D2D1_COLOR_F accent = resources.Colors().accent;
    accent.a = 0.16F;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(context.CreateSolidColorBrush(accent, &brush))) context.FillRoundedRectangle(Rounded(rect, radius), brush.Get());
}

bool IsLeapYear(int year) noexcept { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int DaysInMonth(int year, int month) noexcept {
    static constexpr int kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && IsLeapYear(year)) return 29;
    return kLengths[month - 1];
}

// 0 = Sunday. DaysFromCivil(1970, 1, 1) is day 0 and that day was a Thursday (index 4), hence the
// +11 before the modulo: it maps day 0 to 4 and stays non-negative for every date we can show.
int WeekdayIndex(int year, int month, int day) noexcept {
    const int days = TimeRanges::DaysFromCivil(year, month, day);
    return ((days % 7) + 11) % 7;
}

std::wstring FormatDate(int year, int month, int day) {
    wchar_t buffer[16]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%04d-%02d-%02d", year, month, day);
    return std::wstring(buffer);
}

}  // namespace

void DatePicker::Show(float x, float y, const std::wstring& currentDate) {
    bounds_ = D2D1_RECT_F{x, y, x + kWidth, y + kHeight};
    SYSTEMTIME now{};
    GetLocalTime(&now);
    today_ = Civil{static_cast<int>(now.wYear), static_cast<int>(now.wMonth), static_cast<int>(now.wDay)};
    // Default view: the month of "today", nothing selected.
    view_ = today_;
    selected_ = today_;
    hasSelection_ = false;
    hoveredIndex_ = -1;
    hoveredPart_ = Part::None;
    if (!currentDate.empty()) {
        const auto parsed = TimeRanges::ParseDueDate(Dpi::WideToUtf8(currentDate));
        if (parsed.ok()) {
            view_ = Civil{parsed.value().year, parsed.value().month, parsed.value().day};
            selected_ = view_;
            hasSelection_ = true;
        }
    }
    visible_ = true;
}

void DatePicker::Hide() noexcept {
    visible_ = false;
    hoveredIndex_ = -1;
    hoveredPart_ = Part::None;
}

bool DatePicker::IsOpen() const noexcept { return visible_; }

void DatePicker::SetOnPick(std::function<void(const std::wstring&)> callback) { onPick_ = std::move(callback); }

void DatePicker::ShiftView(int months) noexcept {
    // Convert to a month counter so crossing a year boundary needs no special case, then clamp to
    // a range the Gregorian helpers can render (and which keeps the counter positive).
    constexpr int kMinTotal = 1900 * 12;
    constexpr int kMaxTotal = 2999 * 12 + 11;
    int total = view_.year * 12 + (view_.month - 1) + months;
    if (total < kMinTotal) total = kMinTotal;
    if (total > kMaxTotal) total = kMaxTotal;
    view_.year = total / 12;
    view_.month = total % 12 + 1;
    view_.day = 1;
    hoveredIndex_ = -1;
}

void DatePicker::Pick(int year, int month, int day) {
    selected_ = Civil{year, month, day};
    hasSelection_ = true;
    // Copy the callback before hiding: the callback typically re-enters TaskPage and may call
    // Show() again, which must not observe a half-updated picker.
    auto callback = onPick_;
    Hide();
    if (callback) callback(FormatDate(year, month, day));
}

int DatePicker::CellCountInView() const noexcept { return DaysInMonth(view_.year, view_.month); }

int DatePicker::FirstWeekdayOfView() const noexcept { return WeekdayIndex(view_.year, view_.month, 1); }

float DatePicker::GridTop() const noexcept {
    // Rows actually needed by this month: 4 for a 28-day February starting on Sunday, 6 when the
    // month starts late in the week. Centring the block keeps the gap above the footer looking like
    // padding rather than a missing week.
    const int cells = FirstWeekdayOfView() + CellCountInView();
    const int rows = (cells + kColumns - 1) / kColumns;
    const float area = kFooterTop - kGridTop;
    return kGridTop + (area - static_cast<float>(rows) * kCellHeight) * 0.5F;
}

D2D1_RECT_F DatePicker::CellRect(int row, int column) const noexcept {
    const float width = CellWidth();
    const float left = bounds_.left + kPadding + static_cast<float>(column) * width;
    const float top = bounds_.top + GridTop() + static_cast<float>(row) * kCellHeight;
    return D2D1_RECT_F{left, top, left + width, top + kCellHeight};
}

D2D1_RECT_F DatePicker::PreviousRect() const noexcept {
    return D2D1_RECT_F{bounds_.left + kPadding, bounds_.top + kHeaderTop, bounds_.left + kPadding + kArrowWidth, bounds_.top + kHeaderTop + kHeaderHeight};
}

D2D1_RECT_F DatePicker::NextRect() const noexcept {
    return D2D1_RECT_F{bounds_.right - kPadding - kArrowWidth, bounds_.top + kHeaderTop, bounds_.right - kPadding, bounds_.top + kHeaderTop + kHeaderHeight};
}

D2D1_RECT_F DatePicker::TodayRect() const noexcept {
    return D2D1_RECT_F{bounds_.left + kPadding, bounds_.top + kFooterTop, bounds_.left + kPadding + kFooterButtonWidth, bounds_.top + kFooterTop + kFooterHeight};
}

D2D1_RECT_F DatePicker::ClearRect() const noexcept {
    return D2D1_RECT_F{bounds_.right - kPadding - kFooterButtonWidth, bounds_.top + kFooterTop, bounds_.right - kPadding, bounds_.top + kFooterTop + kFooterHeight};
}

int DatePicker::HitCellIndex(float x, float y) const noexcept {
    if (!visible_) return -1;
    const float left = bounds_.left + kPadding;
    const float top = bounds_.top + GridTop();
    const float width = CellWidth();
    if (x < left || y < top) return -1;
    const int column = static_cast<int>((x - left) / width);
    const int row = static_cast<int>((y - top) / kCellHeight);
    if (column < 0 || column >= kColumns || row < 0 || row >= kRows) return -1;
    return row * kColumns + column;
}

DatePicker::Part DatePicker::HitPart(float x, float y) const noexcept {
    if (!visible_) return Part::None;
    if (Contains(PreviousRect(), x, y)) return Part::PreviousMonth;
    if (Contains(NextRect(), x, y)) return Part::NextMonth;
    if (Contains(TodayRect(), x, y)) return Part::Today;
    if (Contains(ClearRect(), x, y)) return Part::Clear;
    return Part::None;
}

void DatePicker::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    resources.DrawGlassPanel(bounds_, 12.0F, true);
    PaintHeader(context, resources);
    PaintWeekdays(context, resources);
    PaintGrid(context, resources);
    PaintFooter(context, resources);
}

void DatePicker::PaintNavButton(ID2D1DeviceContext& context, RenderResources& resources, const D2D1_RECT_F& rect, const wchar_t* glyph, Part part, bool large) {
    if (hoveredPart_ == part) FillHover(context, resources, rect, 8.0F);
    resources.DrawText(glyph, rect, resources.TextBrush(), large, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void DatePicker::PaintHeader(ID2D1DeviceContext& context, RenderResources& resources) {
    const std::wstring title = std::to_wstring(view_.year) + L" 年 " + std::to_wstring(view_.month) + L" 月";
    const D2D1_RECT_F titleRect{bounds_.left + kPadding + kArrowWidth, bounds_.top + kHeaderTop, bounds_.right - kPadding - kArrowWidth, bounds_.top + kHeaderTop + kHeaderHeight};
    resources.DrawText(title, titleRect, resources.TextBrush(), true, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    // U+2039 / U+203A single guillemets read as chevrons in Microsoft YaHei UI.
    PaintNavButton(context, resources, PreviousRect(), L"\u2039", Part::PreviousMonth, true);
    PaintNavButton(context, resources, NextRect(), L"\u203A", Part::NextMonth, true);
}

void DatePicker::PaintWeekdays(ID2D1DeviceContext& context, RenderResources& resources) {
    static constexpr const wchar_t* kLabels[kColumns] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    const float width = CellWidth();
    const D2D1_RECT_F row{bounds_.left + kPadding, bounds_.top + kWeekdayTop, bounds_.right - kPadding, bounds_.top + kWeekdayTop + kWeekdayHeight};
    for (int column = 0; column < kColumns; ++column) {
        const float left = row.left + static_cast<float>(column) * width;
        const D2D1_RECT_F cell{left, row.top, left + width, row.bottom};
        resources.DrawText(kLabels[column], cell, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Thin separator so the weekday band does not melt into the grid.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> separator;
    if (SUCCEEDED(context.CreateSolidColorBrush(resources.Colors().border, &separator))) {
        const float y = bounds_.top + kWeekdayTop + kWeekdayHeight - 0.5F;
        context.FillRectangle(D2D1_RECT_F{row.left, y, row.right, y + 1.0F}, separator.Get());
    }
}

void DatePicker::PaintGrid(ID2D1DeviceContext& context, RenderResources& resources) {
    const int daysInMonth = CellCountInView();
    const int firstWeekday = FirstWeekdayOfView();
    // White text for the selected day: the accent fill is orange on both palettes, so the palette
    // text colour (near-black on the light theme) would be unreadable on it.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectedText;
    if (FAILED(context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &selectedText))) return;
    for (int index = 0; index < kCellCount; ++index) {
        const int day = index - firstWeekday + 1;
        // Leading / trailing cells belong to the neighbouring months and stay deliberately blank so
        // they can never be clicked by accident.
        if (day < 1 || day > daysInMonth) continue;
        const D2D1_RECT_F cell = CellRect(index / kColumns, index % kColumns);
        const D2D1_RECT_F pill{cell.left + 2.0F, cell.top + 2.0F, cell.right - 2.0F, cell.bottom - 2.0F};
        const bool isSelected = hasSelection_ && selected_.year == view_.year && selected_.month == view_.month && selected_.day == day;
        const bool isToday = today_.year == view_.year && today_.month == view_.month && today_.day == day;
        if (isSelected) context.FillRoundedRectangle(Rounded(pill, 8.0F), resources.AccentBrush());
        else if (hoveredIndex_ == index) FillHover(context, resources, pill, 8.0F);
        // "Today" keeps a thin accent outline; it is skipped on the selected day because the solid
        // accent fill already marks it.
        if (isToday && !isSelected) context.DrawRoundedRectangle(Rounded(pill, 8.0F), resources.AccentBrush(), 1.0F);
        resources.DrawText(std::to_wstring(day), cell, isSelected ? selectedText.Get() : resources.TextBrush(), false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void DatePicker::PaintFooter(ID2D1DeviceContext& context, RenderResources& resources) {
    const D2D1_RECT_F today = TodayRect();
    const D2D1_RECT_F clear = ClearRect();
    if (hoveredPart_ == Part::Today) FillHover(context, resources, today, 8.0F);
    context.DrawRoundedRectangle(Rounded(today, 8.0F), resources.BorderBrush(), 1.0F);
    resources.DrawText(L"今天", today, resources.AccentBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (hoveredPart_ == Part::Clear) FillHover(context, resources, clear, 8.0F);
    context.DrawRoundedRectangle(Rounded(clear, 8.0F), resources.BorderBrush(), 1.0F);
    resources.DrawText(L"清空", clear, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
}

bool DatePicker::OnMouseMove(float x, float y) {
    if (!visible_) return false;
    if (!HitTest(x, y)) {
        hoveredIndex_ = -1;
        hoveredPart_ = Part::None;
        return true;
    }
    hoveredPart_ = HitPart(x, y);
    hoveredIndex_ = -1;
    if (hoveredPart_ == Part::None) {
        const int index = HitCellIndex(x, y);
        if (index >= 0) {
            const int day = index - FirstWeekdayOfView() + 1;
            if (day >= 1 && day <= CellCountInView()) hoveredIndex_ = index;
        }
    }
    return true;
}

bool DatePicker::OnMouseUp(float x, float y) {
    if (!visible_) return false;
    const Part part = HitPart(x, y);
    if (part == Part::PreviousMonth) { ShiftView(-1); return true; }
    if (part == Part::NextMonth) { ShiftView(1); return true; }
    if (part == Part::Today) { Pick(today_.year, today_.month, today_.day); return true; }
    if (part == Part::Clear) {
        // Clearing must not leave a stale selection behind, otherwise reopening would highlight a
        // day that is no longer the due date.
        hasSelection_ = false;
        auto callback = onPick_;
        Hide();
        if (callback) callback(std::wstring());
        return true;
    }
    const int index = HitCellIndex(x, y);
    if (index >= 0) {
        const int day = index - FirstWeekdayOfView() + 1;
        if (day >= 1 && day <= CellCountInView()) { Pick(view_.year, view_.month, day); return true; }
    }
    // A click anywhere else inside the popup is consumed but does nothing; a click outside closes
    // it (the click itself is still swallowed so the control underneath is not activated twice).
    if (!HitTest(x, y)) Hide();
    return true;
}

bool DatePicker::OnKeyDown(UINT key, bool, bool) {
    if (!visible_) return false;
    if (key == VK_ESCAPE) { Hide(); return true; }
    if (key == VK_LEFT) { ShiftView(-1); return true; }
    if (key == VK_RIGHT) { ShiftView(1); return true; }
    return false;
}

}  // namespace todo::ui
