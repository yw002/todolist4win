#pragma once

#include <string>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "ui/Theme.h"

namespace todo {

class RenderResources {
public:
    bool Create(ID2D1DeviceContext* context, IDWriteFactory* dwrite, float dpi);
    void Release() noexcept;
    void ApplyTheme(const ThemePalette& theme);
    // NOTE: the secondary-text flag is deliberately NOT named "small". <rpcndr.h> (pulled in by
    // windows.h) contains "#define small char"; a parameter named "small" expands to "bool char"
    // and fails to compile (C2632). Keep the name distinct from that macro.
    void DrawText(const std::wstring& text, const D2D1_RECT_F& rect, ID2D1Brush* brush = nullptr, bool title = false, bool secondary = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    void DrawGlassPanel(const D2D1_RECT_F& rect, float radius = 14.0F, bool strong = false);
    void DrawRoundRect(const D2D1_RECT_F& rect, float radius, ID2D1Brush* fill, ID2D1Brush* border = nullptr, float stroke = 1.0F);
    [[nodiscard]] ID2D1SolidColorBrush* TextBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* MutedBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* AccentBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* DangerBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* SuccessBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* WarningBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* PanelBrush() const noexcept;
    [[nodiscard]] ID2D1SolidColorBrush* BorderBrush() const noexcept;
    [[nodiscard]] IDWriteTextFormat* BodyFormat() const noexcept;
    [[nodiscard]] float MeasureTextWidth(const std::wstring& text, float maxWidth = 4096.0F) const;
    [[nodiscard]] float Dpi() const noexcept;
    [[nodiscard]] const ThemePalette& Colors() const noexcept;

private:
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> danger_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> success_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> warning_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panel_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_;
    ThemePalette colors_{};
    float dpi_{96.0F};
};

}  // namespace todo
