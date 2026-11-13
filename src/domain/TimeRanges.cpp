#include "domain/TimeRanges.h"

#include <cstdio>
#include <string>

namespace todo::TimeRanges {

bool IsValidDate(int year, int month, int day) noexcept {
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum = lengths[month - 1];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leap) maximum = 29;
    return day <= maximum;
}

int DaysFromCivil(int year, int month, int day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

CivilDate CivilFromDays(int days) noexcept {
    days += 719468;
    const int era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int year = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    return {year + (month <= 2), static_cast<int>(month), static_cast<int>(day)};
}

LocalDateTime StartOfDay(const LocalDateTime& value) noexcept {
    LocalDateTime result = value;
    result.hour = result.minute = result.second = result.microsecond = 0;
    return result;
}

LocalDateTime AddDays(const LocalDateTime& value, int days) noexcept {
    const auto date = CivilFromDays(DaysFromCivil(value.year, value.month, value.day) + days);
    LocalDateTime result = value;
    result.year = date.year;
    result.month = date.month;
    result.day = date.day;
    return result;
}

LocalDateTime EndOfDate(const LocalDateTime& value) noexcept {
    LocalDateTime result = StartOfDay(value);
    result.hour = 23;
    result.minute = 59;
    result.second = 59;
    result.microsecond = 0;
    return result;
}

TimeRange Today(const LocalDateTime& now) noexcept {
    const auto start = StartOfDay(now);
    return {start, AddDays(start, 1)};
}

TimeRange SevenDays(const LocalDateTime& now) noexcept {
    const auto today = Today(now);
    return {AddDays(today.start, -6), today.end};
}

bool IsOverdue(const Task& task, const LocalDateTime& now) noexcept {
    return !task.completed && task.dueAt && *task.dueAt < now;
}

Result<LocalDateTime> ParseDueDate(const std::string& yyyyMmDd) {
    if (yyyyMmDd.size() != 10 || yyyyMmDd[4] != '-' || yyyyMmDd[7] != '-') {
        return Result<LocalDateTime>::Failure({ErrorCode::InvalidInput, "截止日期格式应为 YYYY-MM-DD"});
    }
    for (std::size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (yyyyMmDd[i] < '0' || yyyyMmDd[i] > '9') return Result<LocalDateTime>::Failure({ErrorCode::InvalidInput, "截止日期格式应为 YYYY-MM-DD"});
    }
    const int year = std::stoi(yyyyMmDd.substr(0, 4));
    const int month = std::stoi(yyyyMmDd.substr(5, 2));
    const int day = std::stoi(yyyyMmDd.substr(8, 2));
    if (!IsValidDate(year, month, day)) return Result<LocalDateTime>::Failure({ErrorCode::InvalidInput, "截止日期不是有效日期"});
    return Result<LocalDateTime>::Success(EndOfDate(LocalDateTime{year, month, day}));
}

std::string DateString(const LocalDateTime& value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", value.year, value.month, value.day);
    return buffer;
}

}  // namespace todo::TimeRanges
