#pragma once

#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace todo {

struct LocalDateTime {
    int year{1970};
    int month{1};
    int day{1};
    int hour{0};
    int minute{0};
    int second{0};
    int microsecond{0};

    auto operator<=>(const LocalDateTime&) const = default;
    [[nodiscard]] std::string ToString() const {
        char buffer[40]{};
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06d", year, month, day, hour, minute, second, microsecond);
        return buffer;
    }
    [[nodiscard]] static std::optional<LocalDateTime> Parse(const std::string& text) {
        if (text.size() != 19 && text.size() != 26) return std::nullopt;
        LocalDateTime value{};
        int parsed = 0;
        if (text.size() == 19) {
            parsed = std::sscanf(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &value.year, &value.month, &value.day, &value.hour, &value.minute, &value.second);
            if (parsed != 6) return std::nullopt;
        } else {
            parsed = std::sscanf(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d.%6d", &value.year, &value.month, &value.day, &value.hour, &value.minute, &value.second, &value.microsecond);
            if (parsed != 7) return std::nullopt;
        }
        if (value.year < 1 || value.year > 9999 || value.month < 1 || value.month > 12 || value.day < 1 || value.hour < 0 || value.minute < 0 || value.second < 0 || value.microsecond < 0 || value.hour > 23 || value.minute > 59 || value.second > 59 || value.microsecond > 999999) return std::nullopt;
        static constexpr int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int maximumDay = monthDays[value.month - 1];
        if (value.month == 2 && value.year % 4 == 0 && (value.year % 100 != 0 || value.year % 400 == 0)) maximumDay = 29;
        if (value.day > maximumDay) return std::nullopt;
        if (value.ToString().substr(0, text.size()) != text) return std::nullopt;
        return value;
    }
};

enum class Priority { High, Medium, Low };
enum class Route { AllTasks, Today, Overdue, Category, History, Statistics };
enum class CompletionDisplay { KeepVisible, Archive };
enum class OperationType { Complete, Uncomplete };
enum class ConnectionState { Initializing, Online, Degraded, Reconnecting, Error };
enum class Overlay { None, TaskEditor, CategoryEditor, Settings, Confirm };
enum class CompletionFilter { All, Pending, Completed };
enum class ThemeMode { System, Light, Dark };

struct Category {
    std::int64_t id{0};
    std::string name;
    std::string color{"#007AFF"};
    LocalDateTime createdAt;
};

struct Task {
    std::int64_t id{0};
    std::optional<std::int64_t> categoryId;
    std::string title;
    std::string note;
    // Verbatim body_doc JSON; an empty string means SQL NULL (legacy row, lazily upgraded in memory).
    // Parsing is deliberately NOT done here: a corrupt or newer-version document must never make a
    // task row unreadable. The UI parses and degrades to FromLegacyNote(note) on failure.
    std::string bodyDoc;
    Priority priority{Priority::Medium};
    std::optional<LocalDateTime> dueAt;
    bool completed{false};
    LocalDateTime createdAt;
    std::optional<LocalDateTime> completedAt;
    double sortWeight{0.0};
    std::uint64_t revision{1};
};

struct TaskSnapshot {
    std::string title;
    std::string note;
    Priority priority{Priority::Medium};
    std::optional<LocalDateTime> dueAt;
    std::optional<std::int64_t> originalCategoryId;
    std::string originalCategoryName;
    std::string originalCategoryColor;
    LocalDateTime taskCreatedAt;
};

struct HistoryEvent {
    std::int64_t id{0};
    std::int64_t taskId{0};
    TaskSnapshot snapshot;
    LocalDateTime eventAt;
    std::optional<LocalDateTime> completedAt;
    OperationType operationType{OperationType::Complete};
};

struct TimeRange {
    LocalDateTime start;
    LocalDateTime end;
};

struct TaskFilter {
    std::string keyword;
    std::optional<Priority> priority;
    CompletionFilter completion{CompletionFilter::All};
    std::optional<TimeRange> dueRange;
};

struct HistoryFilter {
    std::string keyword;
    std::optional<std::int64_t> categoryId;
    std::optional<TimeRange> timeRange;
};

struct TaskCursor {
    std::optional<LocalDateTime> dueAt;
    double sortWeight{0.0};
    std::int64_t id{0};
    std::string signature;
    // Freezes the query's wall-clock time for the whole pagination session. A continuation page
    // must reuse this value for query.now, otherwise the Overdue/Today signature changes between
    // pages and the repository rejects the cursor.
    std::optional<LocalDateTime> sessionNow;
};

struct HistoryCursor {
    LocalDateTime eventAt;
    std::int64_t id{0};
    std::string signature;
};

template <typename TCursor>
struct PageRequest {
    int pageSize{200};
    std::optional<TCursor> cursor;
};

struct TaskQuery {
    Route route{Route::AllTasks};
    std::optional<std::int64_t> categoryId;
    TaskFilter filter;
    CompletionDisplay display{CompletionDisplay::KeepVisible};
    PageRequest<TaskCursor> page;
    LocalDateTime now;
};

struct HistoryQuery {
    HistoryFilter filter;
    PageRequest<HistoryCursor> page;
};

struct TaskPage {
    std::vector<Task> items;
    std::optional<TaskCursor> nextCursor;
    std::string signature;
};

struct HistoryPage {
    std::vector<HistoryEvent> items;
    std::optional<HistoryCursor> nextCursor;
    std::string signature;
};

struct CategoryCount {
    Category category;
    std::int64_t total{0};
    std::int64_t pending{0};
    std::int64_t completed{0};
};

struct CategoryStat {
    std::optional<std::int64_t> categoryId;
    std::string name;
    std::string color;
    std::int64_t total{0};
    std::int64_t completed{0};
};

struct DayCount {
    LocalDateTime day;
    std::int64_t completedEvents{0};
};

struct Overview {
    std::int64_t pending{0};
    std::int64_t completed{0};
    std::int64_t overdue{0};
    std::int64_t completedToday{0};
    std::int64_t todayPending{0};
};

struct StatsSnapshot {
    double completionRate{0.0};
    std::vector<CategoryStat> categories;
    std::vector<DayCount> sevenDayTrend;
    Overview overview;
};

struct BatchItemResult {
    std::int64_t id{0};
    bool success{false};
    std::string message;
};

struct BatchResult {
    std::vector<BatchItemResult> items;
    [[nodiscard]] int Succeeded() const noexcept {
        int count = 0;
        for (const auto& item : items) if (item.success) ++count;
        return count;
    }
    [[nodiscard]] int Failed() const noexcept { return static_cast<int>(items.size()) - Succeeded(); }
};

[[nodiscard]] inline std::string ToString(Priority value) {
    switch (value) { case Priority::High: return "HIGH"; case Priority::Medium: return "MEDIUM"; case Priority::Low: return "LOW"; }
    return "MEDIUM";
}
[[nodiscard]] inline std::optional<Priority> PriorityFromString(const std::string& value) {
    if (value == "HIGH") return Priority::High;
    if (value == "MEDIUM") return Priority::Medium;
    if (value == "LOW") return Priority::Low;
    return std::nullopt;
}
[[nodiscard]] inline std::string ToString(OperationType value) { return value == OperationType::Complete ? "COMPLETE" : "UNCOMPLETE"; }
[[nodiscard]] inline std::optional<OperationType> OperationTypeFromString(const std::string& value) {
    if (value == "COMPLETE") return OperationType::Complete;
    if (value == "UNCOMPLETE") return OperationType::Uncomplete;
    return std::nullopt;
}

}  // namespace todo
