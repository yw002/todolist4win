#include "ui/controls/TextBox.h"

#include <imm.h>

#include <algorithm>
#include <cwchar>
#include <cstring>

#include <d2d1_1helper.h>
#include <functional>
#include <wrl/client.h>

namespace todo::ui {
namespace {
bool IsControlCharacter(wchar_t c) { return c < 32 && c != L'\t' && c != L'\r' && c != L'\n'; }
}

TextBox::TextBox() = default;
void TextBox::SetText(const std::wstring& text, bool notify) { text_ = text; caret_ = anchor_ = std::min(caret_, text_.size()); composition_.clear(); if (notify) Changed(); }
void TextBox::SetPlaceholder(std::wstring text) { placeholder_ = std::move(text); }
void TextBox::SetPassword(bool password) noexcept { password_ = password; }
void TextBox::SetMultiline(bool multiline) noexcept { multiline_ = multiline; }
void TextBox::SetReadOnly(bool readOnly) noexcept { readOnly_ = readOnly; }
void TextBox::SetOnChanged(std::function<void(const std::wstring&)> callback) { onChanged_ = std::move(callback); }
void TextBox::SetOnSubmit(std::function<void()> callback) { onSubmit_ = std::move(callback); }
const std::wstring& TextBox::Text() const noexcept { return text_; }
std::size_t TextBox::Caret() const noexcept { return caret_; }
void TextBox::SelectAll() noexcept { anchor_ = 0; caret_ = text_.size(); }
TextBox::Selection TextBox::SelectedRange() const noexcept { return {std::min(anchor_, caret_), std::max(anchor_, caret_)}; }
void TextBox::SaveUndo() { if (!undo_.empty() && undo_.back() == text_) return; undo_.push_back(text_); if (undo_.size() > 32) undo_.erase(undo_.begin()); }
void TextBox::Changed() { if (onChanged_) onChanged_(text_); }
void TextBox::EraseSelection() {
    const auto selection = SelectedRange();
    if (selection.start == selection.end) return;
    text_.erase(selection.start, selection.end - selection.start);
    caret_ = anchor_ = selection.start;
}
void TextBox::Insert(const std::wstring& value) {
    if (readOnly_ || value.empty()) return;
    SaveUndo();
    EraseSelection();
    text_.insert(caret_, value);
    caret_ += value.size();
    anchor_ = caret_;
    Changed();
}

std::wstring TextBox::DisplayText() const {
    if (!password_) return text_;
    return std::wstring(text_.size(), L'●');
}

float TextBox::CaretX(std::size_t index, RenderResources& resources) const {
    const std::wstring display = DisplayText();
    return bounds_.left + 10.0F + resources.MeasureTextWidth(display.substr(0, std::min(index, display.size())), std::max(1.0F, bounds_.right - bounds_.left - 20.0F));
}

std::size_t TextBox::IndexAt(float x, RenderResources* resources) {
    const std::wstring display = DisplayText();
    // The pointer is stable across device rebuilds, but its brushes/formats are momentarily empty
    // between Release() and Create(). Bail out instead of measuring with a released text format.
    if (!resources || !resources->BodyFormat()) return caret_;
    std::size_t low = 0, high = display.size();
    const float relative = x - bounds_.left - 10.0F;
    while (low < high) {
        const std::size_t middle = (low + high + 1) / 2;
        if (resources->MeasureTextWidth(display.substr(0, middle), std::max(1.0F, bounds_.right - bounds_.left - 20.0F)) <= relative) low = middle;
        else high = middle - 1;
    }
    return low;
}

void TextBox::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    paintResources_ = &resources;
    // Focus outranks hover: a focused field must keep the accent border, otherwise the caret would
    // sit in a field that looks less active than the one under the pointer.
    const bool hover = enabled_ && hovered_ && !focused_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    D2D1_COLOR_F color = resources.Colors().panelStrong;
    if (focused_) { color.r = color.r * 0.94F + 0.06F; color.g = color.g * 0.94F + 0.06F; color.b = color.b * 0.94F + 0.06F; }
    else if (hover) { color.r = color.r * 0.94F + 0.06F; color.g = color.g * 0.94F + 0.06F; color.b = color.b * 0.94F + 0.06F; }
    if (!enabled_) color.a *= 0.5F;
    context.CreateSolidColorBrush(color, &background);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBorder;
    if (hover) {
        D2D1_COLOR_F borderColor = resources.Colors().accent;
        borderColor.a = 0.35F;
        context.CreateSolidColorBrush(borderColor, &hoverBorder);
    }
    resources.DrawRoundRect(bounds_, 9.0F, background.Get(),
                            focused_ ? resources.AccentBrush() : hover ? hoverBorder.Get() : resources.BorderBrush(),
                            focused_ ? 1.5F : hover ? 1.3F : 1.0F);
    const D2D1_RECT_F inner{bounds_.left + 10, bounds_.top + 2, bounds_.right - 10, bounds_.bottom - 2};
    context.PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const auto selection = SelectedRange();
    if (focused_ && selection.start != selection.end) {
        const float start = CaretX(selection.start, resources);
        const float end = CaretX(selection.end, resources);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectBrush;
        context.CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue, 0.28F), &selectBrush);
        context.FillRectangle({start, inner.top + 3, end, inner.bottom - 3}, selectBrush.Get());
    }
    const std::wstring display = DisplayText();
    if (display.empty() && composition_.empty()) resources.DrawText(placeholder_, inner, resources.MutedBrush());
    else {
        if (multiline_) resources.BodyFormat()->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        const std::size_t insertion = std::min(caret_, display.size());
        resources.DrawText(display.substr(0, insertion) + composition_ + display.substr(insertion), inner, resources.TextBrush());
        if (multiline_) resources.BodyFormat()->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (focused_ && !readOnly_) {
        const float x = CaretX(caret_, resources) + (composition_.empty() ? 0.0F : resources.MeasureTextWidth(composition_));
        context.DrawLine({x, inner.top + 5}, {x, inner.bottom - 5}, resources.AccentBrush(), 1.0F);
    }
    context.PopAxisAlignedClip();
}

bool TextBox::OnMouseDown(float x, float y, UINT keyFlags) {
    if (!enabled_ || !HitTest(x, y)) return false;
    selecting_ = true;
    const std::size_t hit = IndexAt(x, paintResources_);
    caret_ = hit;
    if ((keyFlags & MK_SHIFT) == 0) anchor_ = hit;
    return true;
}
bool TextBox::OnMouseMove(float x, float y) { Control::OnMouseMove(x, y); if (selecting_) { caret_ = IndexAt(x, paintResources_); return true; } return false; }
bool TextBox::OnMouseUp(float, float) { if (!selecting_) return false; selecting_ = false; return true; }

bool TextBox::OnKeyDown(UINT key, bool ctrl, bool shift) {
    if (!focused_ || !enabled_) return false;
    if (ctrl) {
        switch (key) {
            case 'A': SelectAll(); return true;
            case 'C': return Copy();
            case 'X': return Cut();
            case 'V': return Paste();
            case 'Z': if (!undo_.empty() && !readOnly_) { text_ = undo_.back(); undo_.pop_back(); caret_ = anchor_ = text_.size(); Changed(); } return true;
            default: return false;
        }
    }
    const auto selection = SelectedRange();
    switch (key) {
        case VK_ESCAPE:
            if (!readOnly_ && !text_.empty()) { SaveUndo(); text_.clear(); caret_ = anchor_ = 0; Changed(); }
            return true;
        case VK_LEFT:
            if (selection.start != selection.end && !shift) caret_ = selection.start;
            else if (caret_ > 0) --caret_;
            if (!shift) anchor_ = caret_;
            return true;
        case VK_RIGHT:
            if (selection.start != selection.end && !shift) caret_ = selection.end;
            else if (caret_ < text_.size()) ++caret_;
            if (!shift) anchor_ = caret_;
            return true;
        case VK_HOME: caret_ = 0; if (!shift) anchor_ = caret_; return true;
        case VK_END: caret_ = text_.size(); if (!shift) anchor_ = caret_; return true;
        case VK_BACK:
            if (!readOnly_) { SaveUndo(); if (selection.start != selection.end) EraseSelection(); else if (caret_ > 0) { text_.erase(--caret_, 1); anchor_ = caret_; } Changed(); }
            return true;
        case VK_DELETE:
            if (!readOnly_) { SaveUndo(); if (selection.start != selection.end) EraseSelection(); else if (caret_ < text_.size()) text_.erase(caret_, 1); anchor_ = caret_; Changed(); }
            return true;
        case VK_RETURN:
            if (multiline_) Insert(L"\n"); else if (onSubmit_) onSubmit_();
            return true;
        default: return false;
    }
}

bool TextBox::OnChar(wchar_t value) {
    if (!focused_ || !enabled_ || value == L'\r' || value == L'\n' || IsControlCharacter(value)) return false;
    Insert(std::wstring(1, value));
    return true;
}
void TextBox::OnFocus(bool focused) { Control::OnFocus(focused); if (!focused) { selecting_ = false; composition_.clear(); } }

bool TextBox::Copy() const {
    const auto selection = SelectedRange();
    if (selection.start == selection.end || password_) return false;
    if (!OpenClipboard(nullptr)) return false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }
    const std::wstring value = text_.substr(selection.start, selection.end - selection.start);
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) { std::memcpy(target, value.c_str(), bytes); GlobalUnlock(memory); SetClipboardData(CF_UNICODETEXT, memory); memory = nullptr; }
    }
    if (memory) GlobalFree(memory);
    CloseClipboard();
    return true;
}

bool TextBox::Cut() { if (readOnly_ || password_ || !Copy()) return false; SaveUndo(); EraseSelection(); Changed(); return true; }
bool TextBox::Paste() {
    if (readOnly_ || !OpenClipboard(nullptr)) return false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const auto* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    if (text) Insert(text);
    if (text) GlobalUnlock(data);
    CloseClipboard();
    return text != nullptr;
}

bool TextBox::HandleImeStart(HWND hwnd) {
    // A read-only box must not open a composition window; consuming the message keeps the IME quiet.
    if (readOnly_) return true;
    return hwnd != nullptr && focused_;
}
bool TextBox::HandleImeComposition(HWND hwnd, LPARAM lParam) {
    if (readOnly_) { composition_.clear(); return true; }
    if (!focused_) return false;
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return false;
    if ((lParam & GCS_RESULTSTR) != 0) {
        const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring result(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(imc, GCS_RESULTSTR, result.data(), bytes);
            Insert(result);
        }
        composition_.clear();
    } else if ((lParam & GCS_COMPSTR) != 0) {
        const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
        composition_.assign(bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
        if (bytes > 0) ImmGetCompositionStringW(imc, GCS_COMPSTR, composition_.data(), bytes);
    }
    ImmReleaseContext(hwnd, imc);
    return true;
}
bool TextBox::HandleImeEnd(HWND hwnd) { composition_.clear(); if (hwnd) return true; return true; }

void TextBox::UpdateImePosition(HWND hwnd, UINT dpi) {
    if (!hwnd || !focused_) return;
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return;
    COMPOSITIONFORM form{};
    form.dwStyle = CFS_RECT;
    const float scale = static_cast<float>(dpi) / 96.0F;
    form.rcArea = {static_cast<LONG>(bounds_.left * scale), static_cast<LONG>(bounds_.top * scale), static_cast<LONG>(bounds_.right * scale), static_cast<LONG>(bounds_.bottom * scale)};
    form.ptCurrentPos = {form.rcArea.left + 10, form.rcArea.bottom};
    ImmSetCompositionWindow(imc, &form);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = form.ptCurrentPos;
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(hwnd, imc);
}

}  // namespace todo::ui
