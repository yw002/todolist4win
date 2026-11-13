#include <string>

#include "domain/TimeRanges.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;
using namespace todo::TimeRanges;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("TimeRanges", name, value, detail); }
}  // namespace

void RunTimeRangesTests() {
    // --- Calendar validation -------------------------------------------------
    Check("leap year valid", IsValidDate(2024, 2, 29));
    Check("non leap rejected", !IsValidDate(2025, 2, 29));
    Check("century non leap rejected", !IsValidDate(1900, 2, 29));
    Check("400 year leap valid", IsValidDate(2000, 2, 29));
    Check("month rejected", !IsValidDate(2025, 13, 1));
    Check("month zero rejected", !IsValidDate(2025, 0, 1));
    Check("day zero rejected", !IsValidDate(2025, 1, 0));
    Check("short month day rejected", !IsValidDate(2025, 4, 31));
    Check("year zero rejected", !IsValidDate(0, 1, 1));

    // --- Due date parsing ---------------------------------------------------
    auto due = ParseDueDate("2026-03-10");
    Check("due date parsed", due.ok());
    Check("due at end of day", due.ok() && due.value().hour == 23 && due.value().minute == 59 && due.value().second == 59 &&
                                   due.value().microsecond == 0);
    Check("leap due date parsed", ParseDueDate("2024-02-29").ok());
    Check("bad due rejected", !ParseDueDate("2026-02-30").ok());
    Check("short format rejected", !ParseDueDate("2026-3-10").ok());
    Check("trailing space rejected", !ParseDueDate("2026-03-10 ").ok());
    Check("non numeric rejected", !ParseDueDate("20xx-03-10").ok());
    Check("empty due rejected", !ParseDueDate("").ok());

    // --- Day ranges ---------------------------------------------------------
    const LocalDateTime now{2026, 3, 10, 8, 0, 0};
    const auto today = Today(now);
    Check("today half-open", today.start.day == 10 && today.start.hour == 0 && today.end.day == 11 && today.end.hour == 0);
    const auto monthEnd = Today(LocalDateTime{2026, 3, 31, 23, 30, 0});
    Check("today rolls over month", monthEnd.end.year == 2026 && monthEnd.end.month == 4 && monthEnd.end.day == 1);
    const auto week = SevenDays(now);
    Check("seven days starts six days back", week.start.day == 4 && week.start.month == 3);
    Check("seven days ends tomorrow", week.end.day == 11);
    Check("seven days inclusive count",
          DaysFromCivil(week.end.year, week.end.month, week.end.day) - DaysFromCivil(week.start.year, week.start.month, week.start.day) == 7);

    // --- Day arithmetic -----------------------------------------------------
    const auto yearEnd = AddDays(LocalDateTime{2026, 12, 31, 23, 0, 0}, 1);
    Check("add day crosses year", yearEnd.year == 2027 && yearEnd.month == 1 && yearEnd.day == 1 && yearEnd.hour == 23);
    const auto leapEve = AddDays(LocalDateTime{2024, 2, 28, 0, 0, 0}, 1);
    Check("add day enters leap day", leapEve.month == 2 && leapEve.day == 29);
    const auto previousDay = AddDays(LocalDateTime{2026, 3, 1, 12, 0, 0}, -1);
    Check("subtract day crosses month", previousDay.month == 2 && previousDay.day == 28 && previousDay.hour == 12);
    Check("start of day clears time", StartOfDay(LocalDateTime{2026, 3, 10, 18, 4, 5, 123456}).hour == 0 &&
                                          StartOfDay(LocalDateTime{2026, 3, 10, 18, 4, 5, 123456}).microsecond == 0);
    Check("end of date clears microseconds", EndOfDate(LocalDateTime{2026, 3, 10, 1, 2, 3, 999999}).microsecond == 0);

    // --- Epoch conversion ---------------------------------------------------
    Check("days from civil epoch", DaysFromCivil(1970, 1, 1) == 0);
    const auto epoch = CivilFromDays(0);
    Check("civil from epoch days", epoch.year == 1970 && epoch.month == 1 && epoch.day == 1);
    const auto date = CivilFromDays(DaysFromCivil(2026, 3, 10));
    Check("civil date roundtrip", date.year == 2026 && date.month == 3 && date.day == 10);
    Check("date string padded", DateString(LocalDateTime{2026, 3, 5}) == "2026-03-05");

    // --- Overdue semantics (strictly before now) ----------------------------
    Task task;
    task.completed = false;
    task.dueAt = LocalDateTime{2026, 3, 10, 7, 59, 0};
    Check("overdue before now", IsOverdue(task, now));
    task.dueAt = LocalDateTime{2026, 3, 10, 8, 0, 0};
    Check("not overdue at exact deadline", !IsOverdue(task, now));
    task.dueAt = LocalDateTime{2026, 3, 10, 8, 0, 1};
    Check("not overdue after deadline", !IsOverdue(task, now));
    task.completed = true;
    task.dueAt = LocalDateTime{2026, 3, 10, 7, 0, 0};
    Check("completed not overdue", !IsOverdue(task, now));
    task.completed = false;
    task.dueAt.reset();
    Check("no due date not overdue", !IsOverdue(task, now));
}
