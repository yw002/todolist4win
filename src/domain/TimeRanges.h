#pragma once

#include <optional>
#include <string>

#include "common/Result.h"
#include "domain/Models.h"

namespace todo::TimeRanges {

struct CivilDate { int year; int month; int day; };
[[nodiscard]] bool IsValidDate(int year, int month, int day) noexcept;
[[nodiscard]] int DaysFromCivil(int year, int month, int day) noexcept;
[[nodiscard]] CivilDate CivilFromDays(int days) noexcept;
[[nodiscard]] LocalDateTime StartOfDay(const LocalDateTime& value) noexcept;
[[nodiscard]] LocalDateTime AddDays(const LocalDateTime& value, int days) noexcept;
[[nodiscard]] LocalDateTime EndOfDate(const LocalDateTime& value) noexcept;
[[nodiscard]] TimeRange Today(const LocalDateTime& now) noexcept;
[[nodiscard]] TimeRange SevenDays(const LocalDateTime& now) noexcept;
[[nodiscard]] bool IsOverdue(const Task& task, const LocalDateTime& now) noexcept;
[[nodiscard]] Result<LocalDateTime> ParseDueDate(const std::string& yyyyMmDd);
[[nodiscard]] std::string DateString(const LocalDateTime& value);

}  // namespace todo::TimeRanges
