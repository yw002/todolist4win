#include <cstdint>
#include <optional>
#include <string>

#include "common/Error.h"
#include "domain/Models.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("Models", name, value, detail); }
}  // namespace

void RunModelsTests() {
    // --- LocalDateTime::Parse (used by every repository row reader) ---------
    const auto parsed = LocalDateTime::Parse("2026-03-10 08:30:15");
    Check("timestamp parsed", parsed.has_value());
    Check("parsed fields",
          parsed && parsed->year == 2026 && parsed->month == 3 && parsed->day == 10 && parsed->hour == 8 &&
              parsed->minute == 30 && parsed->second == 15 && parsed->microsecond == 0);

    const auto micro = LocalDateTime::Parse("2026-03-10 08:30:15.123456");
    Check("microseconds parsed", micro && micro->microsecond == 123456);

    Check("reject wrong length", !LocalDateTime::Parse("2026-03-10 08:30").has_value());
    Check("reject trailing data", !LocalDateTime::Parse("2026-03-10 08:30:00 ").has_value());
    Check("reject non canonical text", !LocalDateTime::Parse("2026-3-10  08:30:00").has_value());
    Check("reject invalid day", !LocalDateTime::Parse("2026-02-30 00:00:00").has_value());
    Check("reject invalid hour", !LocalDateTime::Parse("2026-03-10 24:00:00").has_value());
    Check("reject invalid minute", !LocalDateTime::Parse("2026-03-10 08:60:00").has_value());
    Check("reject invalid second", !LocalDateTime::Parse("2026-03-10 08:30:60").has_value());
    Check("reject empty text", !LocalDateTime::Parse("").has_value());
    Check("accept leap day", LocalDateTime::Parse("2024-02-29 00:00:00").has_value());

    const LocalDateTime original{2026, 12, 31, 23, 59, 58, 900000};
    const auto back = LocalDateTime::Parse(original.ToString());
    Check("to string roundtrip", back && *back == original);
    Check("to string is microsecond precision", original.ToString() == "2026-12-31 23:59:58.900000");
    Check("ordering compares fields", LocalDateTime{2026, 1, 2} > LocalDateTime{2026, 1, 1});

    // --- Enum <-> storage string mapping ------------------------------------
    Check("priority string high", ToString(Priority::High) == "HIGH");
    Check("priority string medium", ToString(Priority::Medium) == "MEDIUM");
    Check("priority string low", ToString(Priority::Low) == "LOW");
    Check("priority parsed", PriorityFromString("HIGH") == std::optional<Priority>(Priority::High));
    Check("priority parsing is case sensitive", !PriorityFromString("high").has_value());
    Check("priority rejects unknown value", !PriorityFromString("URGENT").has_value());
    Check("operation string complete", ToString(OperationType::Complete) == "COMPLETE");
    Check("operation string uncomplete", ToString(OperationType::Uncomplete) == "UNCOMPLETE");
    Check("operation parsed", OperationTypeFromString("UNCOMPLETE") == std::optional<OperationType>(OperationType::Uncomplete));
    Check("operation rejects unknown value", !OperationTypeFromString("RESTORE").has_value());

    // --- Batch aggregation (drives the partial failure toast) ---------------
    BatchResult batch;
    batch.items.push_back({1, true, ""});
    batch.items.push_back({2, false, "missing"});
    batch.items.push_back({3, true, ""});
    Check("batch counts successes", batch.Succeeded() == 2);
    Check("batch counts failures", batch.Failed() == 1);
    BatchResult emptyBatch;
    Check("empty batch counts zero", emptyBatch.Succeeded() == 0 && emptyBatch.Failed() == 0);

    // --- Connection failure classification (drives read-only degradation) ---
    AppError connectionError;
    connectionError.code = ErrorCode::ConnectionFailed;
    Check("connection failure is a connection error", connectionError.IsConnectionError());
    AppError timeoutError;
    timeoutError.code = ErrorCode::Timeout;
    Check("timeout is a connection error", timeoutError.IsConnectionError());
    AppError conflictError;
    conflictError.code = ErrorCode::Conflict;
    Check("conflict is not a connection error", !conflictError.IsConnectionError());
    AppError internalError;
    internalError.code = ErrorCode::Internal;
    Check("internal error is not a connection error", !internalError.IsConnectionError());
    AppError successError;
    Check("success carries no error", successError.code == ErrorCode::None && !successError.IsConnectionError());

    // --- Defaults the controller and UI rely on -----------------------------
    PageRequest<TaskCursor> page;
    Check("default page size matches query guard", page.pageSize == 200);
    Check("default page has no cursor", !page.cursor.has_value());
    Task task;
    Check("new task defaults", task.revision == 1 && task.sortWeight == 0.0 && task.priority == Priority::Medium && !task.completed);
    TaskFilter filter;
    Check("default filter shows every completion state", filter.completion == CompletionFilter::All && !filter.priority.has_value() && !filter.dueRange.has_value());
}
