#pragma once

#include <cstdint>
#include <vector>

#include "domain/Models.h"

namespace todo::Statistics {

[[nodiscard]] double CompletionRate(std::int64_t total, std::int64_t completed) noexcept;
[[nodiscard]] std::vector<DayCount> FillMissingDays(const TimeRange& sevenDays, const std::vector<DayCount>& values);

}  // namespace todo::Statistics
