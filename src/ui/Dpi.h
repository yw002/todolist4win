#pragma once

#include <windows.h>

#include <string>

#include <d2d1_1.h>

namespace todo::Dpi {

[[nodiscard]] inline float Scale(UINT dpi) noexcept { return static_cast<float>(dpi) / 96.0F; }
[[nodiscard]] inline int ToPixels(float dip, UINT dpi) noexcept { return static_cast<int>(dip * Scale(dpi) + 0.5F); }
[[nodiscard]] inline float ToDips(int pixels, UINT dpi) noexcept { return static_cast<float>(pixels) / Scale(dpi); }
[[nodiscard]] D2D1_RECT_F AdjustRect(const D2D1_RECT_F& rect, float left, float top, float right, float bottom) noexcept;
[[nodiscard]] std::wstring Utf8ToWide(const std::string& value);
[[nodiscard]] std::string WideToUtf8(const std::wstring& value);

}  // namespace todo::Dpi
