#include "domain/Statistics.h"

#include <algorithm>
#include <map>
#include <vector>

#include "domain/TimeRanges.h"

namespace todo::Statistics {

double CompletionRate(std::int64_t total, std::int64_t completed) noexcept {
    if (total <= 0) return 0.0;
    const double rate = static_cast<double>(completed) / static_cast<double>(total);
    return std::clamp(rate, 0.0, 1.0);
}

std::vector<DayCount> FillMissingDays(const TimeRange& sevenDays, const std::vector<DayCount>& values) {
    std::map<int, std::int64_t> counts;
    for (const auto& value : values) counts[TimeRanges::DaysFromCivil(value.day.year, value.day.month, value.day.day)] += value.completedEvents;
    std::vector<DayCount> result;
    const int start = TimeRanges::DaysFromCivil(sevenDays.start.year, sevenDays.start.month, sevenDays.start.day);
    const int end = TimeRanges::DaysFromCivil(sevenDays.end.year, sevenDays.end.month, sevenDays.end.day);
    for (int day = start; day < end; ++day) {
        const auto date = TimeRanges::CivilFromDays(day);
        result.push_back({LocalDateTime{date.year, date.month, date.day}, counts[day]});
    }
    return result;
}

}  // namespace todo::Statistics
