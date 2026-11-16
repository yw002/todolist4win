#include "config/AppConfig.h"

#include <algorithm>
#include <cmath>

namespace todo {

void AppConfig::Sanitize() noexcept {
    if (!std::isfinite(window.leftDip) || !std::isfinite(window.topDip)) { window.leftDip = 120; window.topDip = 100; }
    if (!std::isfinite(window.widthDip) || !std::isfinite(window.heightDip)) { window.widthDip = 1180; window.heightDip = 760; }
    window.widthDip = std::clamp(window.widthDip, 760.0, 4096.0);
    window.heightDip = std::clamp(window.heightDip, 560.0, 4096.0);
    if (window.lastDpi < 96 || window.lastDpi > 768) window.lastDpi = 96;
}

}  // namespace todo
