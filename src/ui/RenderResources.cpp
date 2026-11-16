#include "ui/RenderResources.h"

#include <d2d1_1helper.h>

namespace todo {

bool RenderResources::Create(ID2D1DeviceContext* context, IDWriteFactory* dwrite, float dpi) {
    Release();
    if (!context || !dwrite) return false;
    context_ = context;
    dwrite_ = dwrite;
    dpi_ = dpi;
    auto brush = [context](auto& target, D2D1_COLOR_F color) { return SUCCEEDED(context->CreateSolidColorBrush(color, &target)); };
    if (!brush(text_, colors_.text) || !brush(muted_, colors_.muted) || !brush(accent_, colors_.accent) || !brush(danger_, colors_.danger) || !brush(success_, colors_.success) || !brush(warning_, colors_.warning) || !brush(panel_, colors_.panel) || !brush(border_, colors_.border)) return false;
    auto format = [dwrite](auto& target, float size, DWRITE_FONT_WEIGHT weight) {
        return SUCCEEDED(dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &target));
    };
    if (!format(body_, 14.0F, DWRITE_FONT_WEIGHT_NORMAL) || !format(title_, 18.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD) || !format(small_, 12.0F, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    for (auto* value : {body_.Get(), title_.Get(), small_.Get()}) {
        value->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        value->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // A separate format instance for the body editor: WRAP + top alignment. The three shared
    // formats above are untouched, so no other control can observe a wrapping state change.
    if (!format(bodyWrap_, 14.0F, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    bodyWrap_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    bodyWrap_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    bodyWrap_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ++deviceGeneration_;  // Create() invalidates derived bitmaps, as does Release() below.
    return true;
}

void RenderResources::Release() noexcept {
    text_.Reset(); muted_.Reset(); accent_.Reset(); danger_.Reset(); success_.Reset(); warning_.Reset(); panel_.Reset(); border_.Reset();
    body_.Reset(); title_.Reset(); small_.Reset(); bodyWrap_.Reset(); context_.Reset(); dwrite_.Reset();
    ++deviceGeneration_;
}

void RenderResources::ApplyTheme(const ThemePalette& theme) {
    colors_ = theme;
    if (text_) text_->SetColor(theme.text);
    if (muted_) muted_->SetColor(theme.muted);
    if (accent_) accent_->SetColor(theme.accent);
    if (danger_) danger_->SetColor(theme.danger);
    if (success_) success_->SetColor(theme.success);
    if (warning_) warning_->SetColor(theme.warning);
    if (panel_) panel_->SetColor(theme.panel);
    if (border_) border_->SetColor(theme.border);
}

void RenderResources::DrawText(const std::wstring& text, const D2D1_RECT_F& rect, ID2D1Brush* brush, bool title, bool secondary, DWRITE_TEXT_ALIGNMENT alignment) {
    if (!context_ || !body_) return;
    IDWriteTextFormat* format = title ? title_.Get() : secondary ? small_.Get() : body_.Get();
    format->SetTextAlignment(alignment);
    context_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush ? brush : text_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RenderResources::DrawRoundRect(const D2D1_RECT_F& rect, float radius, ID2D1Brush* fill, ID2D1Brush* border, float stroke) {
    if (!context_) return;
    const D2D1_ROUNDED_RECT rounded{rect, radius, radius};
    if (fill) context_->FillRoundedRectangle(rounded, fill);
    if (border) context_->DrawRoundedRectangle(rounded, border, stroke);
}

void RenderResources::DrawGlassPanel(const D2D1_RECT_F& rect, float radius, bool strong) {
    if (!context_) return;
    // Outer drop shadow, drawn as a few grown low-alpha rounded rectangles.
    // The previous implementation pushed a 1x1 bitmap through a shadow/blur effect and then tried
    // to stretch the result onto the panel rect - that cannot work: ID2D1DeviceContext::DrawImage
    // has no target-rectangle overload (d2d1_1.h:794 takes a targetOffset point, not a rect), and a
    // genuine frosted backdrop needs the already-composited backdrop, which this app does not have.
    constexpr int kShadowLayers = 5;
    for (int layer = kShadowLayers; layer >= 1; --layer) {
        const float grow = static_cast<float>(layer) * 1.5F;
        D2D1_COLOR_F color{0.06F, 0.04F, 0.025F, 0.06F / static_cast<float>(layer)};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadow;
        if (FAILED(context_->CreateSolidColorBrush(color, &shadow))) break;
        const D2D1_ROUNDED_RECT grown{{rect.left - grow, rect.top - grow + 2.0F, rect.right + grow, rect.bottom + grow + 2.0F}, radius + grow, radius + grow};
        context_->FillRoundedRectangle(grown, shadow.Get());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    if (FAILED(context_->CreateSolidColorBrush(strong ? colors_.popup : colors_.panel, &fill))) return;
    if (FAILED(context_->CreateSolidColorBrush(colors_.border, &border))) return;
    DrawRoundRect(rect, radius, fill.Get(), border.Get());
    // The thin bright top edge is what makes the surface read as glass rather than as a flat card.
    D2D1_COLOR_F edge{1.0F, 1.0F, 1.0F, strong ? 0.18F : 0.10F};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlight;
    if (SUCCEEDED(context_->CreateSolidColorBrush(edge, &highlight))) {
        const D2D1_RECT_F line{rect.left + radius, rect.top + 1.0F, rect.right - radius, rect.top + 2.0F};
        context_->FillRectangle(line, highlight.Get());
    }
}

ID2D1SolidColorBrush* RenderResources::TextBrush() const noexcept { return text_.Get(); }
ID2D1SolidColorBrush* RenderResources::MutedBrush() const noexcept { return muted_.Get(); }
ID2D1SolidColorBrush* RenderResources::AccentBrush() const noexcept { return accent_.Get(); }
ID2D1SolidColorBrush* RenderResources::DangerBrush() const noexcept { return danger_.Get(); }
ID2D1SolidColorBrush* RenderResources::SuccessBrush() const noexcept { return success_.Get(); }
ID2D1SolidColorBrush* RenderResources::WarningBrush() const noexcept { return warning_.Get(); }
ID2D1SolidColorBrush* RenderResources::PanelBrush() const noexcept { return panel_.Get(); }
ID2D1SolidColorBrush* RenderResources::BorderBrush() const noexcept { return border_.Get(); }
IDWriteTextFormat* RenderResources::BodyFormat() const noexcept { return body_.Get(); }
IDWriteTextFormat* RenderResources::BodyWrapFormat() const noexcept { return bodyWrap_.Get(); }

Microsoft::WRL::ComPtr<IDWriteTextLayout> RenderResources::CreateBodyLayout(const std::wstring& text, float maxWidth, float maxHeight) const {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (!dwrite_ || !bodyWrap_ || maxWidth <= 0.0F || maxHeight <= 0.0F) return layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), bodyWrap_.Get(),
                                         maxWidth, maxHeight, &layout))) {
        layout.Reset();
        return layout;
    }
    // Set on the layout itself as well: an IDWriteTextLayout inherits the format's wrapping at
    // creation time, but being explicit makes the hit-test metrics match the painted lines.
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return layout;
}

void RenderResources::DrawTextLayout(const D2D1_POINT_2F& origin, IDWriteTextLayout* layout, ID2D1Brush* brush) const {
    if (!context_ || !layout) return;
    context_->DrawTextLayout(origin, layout, brush ? brush : text_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float RenderResources::MeasureTextWidth(const std::wstring& text, float maxWidth, bool title) const {
    if (!dwrite_ || !body_ || !title_) return 0.0F;
    IDWriteTextFormat* format = title ? title_.Get() : body_.Get();
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, maxWidth, 64.0F, &layout))) return 0.0F;
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics)) ? metrics.widthIncludingTrailingWhitespace : 0.0F;
}
float RenderResources::Dpi() const noexcept { return dpi_; }
const ThemePalette& RenderResources::Colors() const noexcept { return colors_; }

}  // namespace todo
