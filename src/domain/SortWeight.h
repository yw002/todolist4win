#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "common/Result.h"
#include "domain/Models.h"

namespace todo::SortWeight {

inline constexpr double kStep = 1024.0;
inline constexpr double kMinimumGap = 1e-9;
[[nodiscard]] double NewTaskWeight(double currentMaximum) noexcept;
[[nodiscard]] Result<double> Between(std::optional<double> previous, std::optional<double> next);
[[nodiscard]] bool NeedsRenumbering(double previous, double next) noexcept;
[[nodiscard]] Result<void> Renumber(std::vector<Task>& tasks);

}  // namespace todo::SortWeight
