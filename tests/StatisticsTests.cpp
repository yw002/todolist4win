#include <cmath>
#include <cstdint>
#include <vector>

#include "domain/Statistics.h"
#include "domain/TimeRanges.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("Statistics", name, value, detail); }
bool Near(double left, double right) { return std::abs(left - right) < 1e-9; }
}  // namespace

void RunStatisticsTests() {
    // --- Completion rate ----------------------------------------------------
    Check("zero total does not divide", Statistics::CompletionRate(0, 0) == 0.0);
    Check("zero total with events does not divide", Statistics::CompletionRate(0, 5) == 0.0);
    Check("negative total reported as zero", Statistics::CompletionRate(-3, 1) == 0.0);
    Check("half rate", Near(Statistics::CompletionRate(4, 2), 0.5));
    Check("full rate", Near(Statistics::CompletionRate(4, 4), 1.0));
    Check("rate clamped", Statistics::CompletionRate(2, 3) == 1.0);
    Check("single completed task", Near(Statistics::CompletionRate(1, 1), 1.0));

    // --- Seven day trend ----------------------------------------------------
    const LocalDateTime now{2026, 3, 10, 12, 0, 0};
    const auto range = TimeRanges::SevenDays(now);
    const auto days = Statistics::FillMissingDays(range, {{LocalDateTime{2026, 3, 10}, 2}});
    Check("seven days including today", days.size() == 7);
    Check("first day is six days back", days.front().day.year == 2026 && days.front().day.month == 3 && days.front().day.day == 4);
    Check("missing day is zero", days.front().completedEvents == 0);
    Check("today is last day", days.back().day.day == 10);
    Check("today count", days.back().completedEvents == 2);

    // Duplicate rows for the same day must be summed, not replaced.
    const auto summed = Statistics::FillMissingDays(range, {{LocalDateTime{2026, 3, 10}, 2}, {LocalDateTime{2026, 3, 10}, 3}});
    Check("duplicate day rows are summed", summed.size() == 7 && summed.back().completedEvents == 5);

    // Rows outside the range must not create extra days.
    const auto clipped = Statistics::FillMissingDays(range, {{LocalDateTime{2026, 2, 1}, 9}, {LocalDateTime{2026, 4, 1}, 4}});
    Check("out of range days ignored", clipped.size() == 7 && clipped.front().completedEvents == 0 && clipped.back().completedEvents == 0);

    // Empty input still produces a zero filled series (no gaps in the chart).
    const auto empty = Statistics::FillMissingDays(range, {});
    Check("empty input fills every day", empty.size() == 7);
    Check("empty input is all zero",
          empty[0].completedEvents == 0 && empty[3].completedEvents == 0 && empty[6].completedEvents == 0);
}
