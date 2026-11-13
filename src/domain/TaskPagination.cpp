#include "domain/TaskPagination.h"

#include <functional>
#include <optional>
#include <sstream>
#include <string>

#include "domain/TimeRanges.h"

namespace todo::TaskPagination {

std::string TaskQuerySignature(const TaskQuery& query) {
    std::ostringstream out;
    out << static_cast<int>(query.route) << '|' << query.categoryId.value_or(0) << '|' << query.filter.keyword << '|'
        << static_cast<int>(query.filter.completion) << '|' << static_cast<int>(query.display) << '|'
        << (query.filter.priority ? ToString(*query.filter.priority) : "") << '|'
        << (query.route == Route::Today ? TimeRanges::DateString(query.now) : query.route == Route::Overdue ? query.now.ToString() : "");
    if (query.filter.dueRange) {
        out << '|' << query.filter.dueRange->start.ToString() << '|' << query.filter.dueRange->end.ToString();
    }
    return std::to_string(std::hash<std::string>{}(out.str()));
}

Result<LocalDateTime> ResolveTaskQueryNow(bool more, const std::optional<TaskCursor>& cursor, const LocalDateTime& freshNow) {
    if (!more) return Result<LocalDateTime>::Success(freshNow);
    if (!cursor || !cursor->sessionNow) {
        return Result<LocalDateTime>::Failure({ErrorCode::Conflict, "分页会话已失效，请从第一页加载"});
    }
    return Result<LocalDateTime>::Success(*cursor->sessionNow);
}

bool IsTaskCursorCompatible(const TaskQuery& query) {
    const auto& cursor = query.page.cursor;
    if (!cursor) return true;
    if (cursor->signature != TaskQuerySignature(query)) return false;
    return cursor->sessionNow.has_value() && *cursor->sessionNow == query.now;
}

TaskCursor MakeNextTaskCursor(const Task& last, const TaskQuery& query, const std::string& signature) {
    TaskCursor cursor;
    cursor.dueAt = last.dueAt;
    cursor.sortWeight = last.sortWeight;
    cursor.id = last.id;
    cursor.signature = signature;
    cursor.sessionNow = query.now;
    return cursor;
}

}  // namespace todo::TaskPagination
