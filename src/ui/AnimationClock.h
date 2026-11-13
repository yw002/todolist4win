#pragma once

#include <cstdint>

namespace todo {

class AnimationClock {
public:
    AnimationClock();
    [[nodiscard]] double Now() const noexcept;
    [[nodiscard]] double Delta() noexcept;
    [[nodiscard]] static bool AnimationsEnabled() noexcept;
    [[nodiscard]] static float EaseOutBack(float value) noexcept;
private:
    std::int64_t frequency_{0};
    std::int64_t previous_{0};
};

}  // namespace todo
