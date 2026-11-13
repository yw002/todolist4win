#pragma once

#include <cstdint>

#include <d2d1_1.h>

#include "domain/Models.h"

namespace todo {

struct ThemePalette {
    D2D1_COLOR_F window;
    D2D1_COLOR_F panel;
    D2D1_COLOR_F panelStrong;
    // Opaque-ish surface for anything that floats above dense content: menus, dialogs, the calendar,
    // toasts. It has to be its own slot because panelStrong also dresses text fields and list rows,
    // and those must stay translucent - text drawn over a see-through popup is unreadable.
    D2D1_COLOR_F popup;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F muted;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F warning;
    D2D1_COLOR_F success;
    bool dark{false};
};

class Theme {
public:
    Theme();
    void SetMode(ThemeMode mode);
    void RefreshSystemTheme();
    void BeginTransition(ThemeMode mode, double nowSeconds);
    [[nodiscard]] bool Update(double nowSeconds);
    [[nodiscard]] const ThemePalette& Palette() const noexcept;
    [[nodiscard]] ThemeMode Mode() const noexcept;
    [[nodiscard]] static ThemePalette MakePalette(bool dark) noexcept;
    [[nodiscard]] static bool SystemUsesDarkTheme() noexcept;

private:
    ThemeMode mode_{ThemeMode::System};
    ThemePalette current_;
    ThemePalette from_;
    ThemePalette target_;
    double transitionStart_{0.0};
    bool transitioning_{false};
};

}  // namespace todo
