#include "ui/AnimationClock.h"

#include <windows.h>

#include <algorithm>

namespace todo {

AnimationClock::AnimationClock() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    frequency_ = value.QuadPart;
    QueryPerformanceCounter(&value);
    previous_ = value.QuadPart;
}

double AnimationClock::Now() const noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return frequency_ > 0 ? static_cast<double>(value.QuadPart) / static_cast<double>(frequency_) : 0.0;
}

double AnimationClock::Delta() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    const double delta = frequency_ > 0 ? static_cast<double>(value.QuadPart - previous_) / static_cast<double>(frequency_) : 0.0;
    previous_ = value.QuadPart;
    return std::clamp(delta, 0.0, 0.050);
}

bool AnimationClock::AnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && enabled;
}

float AnimationClock::EaseOutBack(float value) noexcept {
    const float t = std::clamp(value, 0.0F, 1.0F) - 1.0F;
    constexpr float c1 = 1.70158F;
    constexpr float c3 = c1 + 1.0F;
    return 1.0F + c3 * t * t * t + c1 * t * t;
}

}  // namespace todo
