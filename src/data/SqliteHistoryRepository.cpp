#include "data/SqliteHistoryRepository.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <sqlite3.h>

#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"
#include "domain/HistoryPagination.h"
#include "domain/TaskRules.h"

namespace todo {
namespace {

constexpr const char* kColumns =
    "id,task_id,title,note,priority,due_time,original_category_id,original_category_name,original_category_color,"
    "task_created_at,completed_at,event_at,operation_type";

using Binder = std::function<void(SqliteStatement&, int&)>;

std::optional<LocalDateTime> OptionalTime(const SqliteStatement& row, int index) {
    if (row.ColumnIsNull(index)) return std::nullopt;
    return LocalDateTime::Parse(row.ColumnText(index));
}

std::optional<HistoryEvent> ReadEvent(SqliteStatement& row) {
    const std::int64_t id = row.ColumnInt64(0);
    const std::int64_t taskId = row.ColumnInt64(1);
    if (id <= 0 || taskId <= 0) return std::nullopt;
    auto priority = PriorityFromString(row.ColumnText(4));
    auto operation = OperationTypeFromString(row.ColumnText(12));
    auto created = LocalDateTime::Parse(row.ColumnText(9));
    auto eventAt = LocalDateTime::Parse(row.ColumnText(11));
    if (!priority || !operation || !created || !eventAt) return std::nullopt;
    HistoryEvent event;
    event.id = id;
    event.taskId = taskId;
    event.snapshot.title = row.ColumnText(2);
    event.snapshot.note = row.ColumnText(3);
    event.snapshot.priority = *priority;
    event.snapshot.dueAt = OptionalTime(row, 5);
    event.snapshot.taskCreatedAt = *created;
    event.completedAt = OptionalTime(row, 10);
    event.eventAt = *eventAt;
    event.operationType = *operation;
    if (!row.ColumnIsNull(6)) event.snapshot.originalCategoryId = row.ColumnInt64(6);
    if (!row.ColumnIsNull(7)) event.snapshot.originalCategoryName = row.ColumnText(7);
    if (!row.ColumnIsNull(8)) event.snapshot.originalCategoryColor = row.ColumnText(8);
    return event;
}

void AppendFilter(std::ostringstream& sql, std::vector<Binder>& binders, const HistoryQuery& query) {
    if (query.filter.categoryId) {
        sql << " AND original_category_id=?";
        binders.push_back([id = *query.filter.categoryId](SqliteStatement& s, int& i) { s.BindInt64(i++, id); });
    }
    if (query.filter.timeRange) {
        sql << " AND event_at>=? AND event_at<?";
        const auto range = *query.filter.timeRange;
        binders.push_back([range](SqliteStatement& s, int& i) {
            s.BindText(i++, range.start.ToString());
            s.BindText(i++, range.end.ToString());
        });
    }
    if (!query.filter.keyword.empty()) {
        sql << " AND (title LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\')";
        const std::string pattern = "%" + TaskRules::EscapeLike(query.filter.keyword) + "%";
        binders.push_back([pattern](SqliteStatement& s, int& i) {
            s.BindText(i++, pattern);
            s.BindText(i++, pattern);
        });
    }
}

}  // namespace

SqliteHistoryRepository::SqliteHistoryRepository(sqlite3* db) : db_(db) {}

Result<HistoryEvent> SqliteHistoryRepository::Append(const HistoryEvent& event) {
    if (db_ == nullptr) return Result<HistoryEvent>::Failure(InternalSqliteError());
    SqliteStatement insert(db_,
                           "INSERT INTO history(task_id,title,note,priority,due_time,original_category_id,original_category_name,"
                           "original_category_color,task_created_at,completed_at,event_at,operation_type)"
                           " VALUES (?,?,?,?,?,?,?,?,?,?,?,?);");
    auto prepared = insert.Prepare();
    if (!prepared.ok()) return Result<HistoryEvent>::Failure(prepared.error());
    insert.BindInt64(1, event.taskId);
    insert.BindText(2, event.snapshot.title);
    insert.BindText(3, event.snapshot.note);
    insert.BindText(4, ToString(event.snapshot.priority));
    if (event.snapshot.dueAt) insert.BindText(5, event.snapshot.dueAt->ToString()); else insert.BindNull(5);
    if (event.snapshot.originalCategoryId) insert.BindInt64(6, *event.snapshot.originalCategoryId); else insert.BindNull(6);
    if (event.snapshot.originalCategoryName.empty()) insert.BindNull(7); else insert.BindText(7, event.snapshot.originalCategoryName);
    if (event.snapshot.originalCategoryColor.empty()) insert.BindNull(8); else insert.BindText(8, event.snapshot.originalCategoryColor);
    insert.BindText(9, event.snapshot.taskCreatedAt.ToString());
    if (event.completedAt) insert.BindText(10, event.completedAt->ToString()); else insert.BindNull(10);
    insert.BindText(11, event.eventAt.ToString());
    insert.BindText(12, ToString(event.operationType));
    auto ran = insert.Run();
    if (!ran.ok()) return Result<HistoryEvent>::Failure(ran.error());
    HistoryEvent stored = event;
    stored.id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
    return Result<HistoryEvent>::Success(std::move(stored));
}

Result<HistoryPage> SqliteHistoryRepository::QueryPage(const HistoryQuery& request) {
    if (db_ == nullptr) return Result<HistoryPage>::Failure(InternalSqliteError());
    const std::string signature = HistoryPagination::QuerySignature(request);
    if (!HistoryPagination::IsCursorCompatible(request)) {
        return Result<HistoryPage>::Failure({ErrorCode::Conflict, "历史筛选条件已变化"});
    }
    std::ostringstream sql;
    sql << "SELECT " << kColumns << " FROM history WHERE 1=1";
    std::vector<Binder> binders;
    AppendFilter(sql, binders, request);
    if (request.page.cursor) {
        const auto cursor = *request.page.cursor;
        sql << " AND (event_at<? OR (event_at=? AND id<?))";
        binders.push_back([cursor](SqliteStatement& s, int& i) {
            s.BindText(i++, cursor.eventAt.ToString());
            s.BindText(i++, cursor.eventAt.ToString());
            s.BindInt64(i++, cursor.id);
        });
    }
    sql << " ORDER BY event_at DESC,id DESC LIMIT ?;";
    SqliteStatement query(db_, sql.str());
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<HistoryPage>::Failure(prepared.error());
    int index = 1;
    for (auto& bind : binders) bind(query, index);
    query.BindInt64(index, request.page.pageSize + 1);
    HistoryPage page;
    page.signature = signature;
    for (;;) {
        auto row = query.Step();
        if (!row.ok()) return Result<HistoryPage>::Failure(row.error());
        if (!row.value()) break;
        auto event = ReadEvent(query);
        if (event) page.items.push_back(std::move(*event));
    }
    if (static_cast<int>(page.items.size()) > request.page.pageSize) {
        page.items.resize(static_cast<std::size_t>(request.page.pageSize));
        page.nextCursor = HistoryPagination::MakeNextHistoryCursor(page.items.back(), request, signature);
    }
    return Result<HistoryPage>::Success(std::move(page));
}

Result<void> SqliteHistoryRepository::DeleteEvents(const std::vector<std::int64_t>& ids) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    if (ids.empty()) return Result<void>::Success();
    std::ostringstream sql;
    sql << "DELETE FROM history WHERE id IN (";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) sql << ',';
        sql << '?';
    }
    sql << ");";
    SqliteStatement remove(db_, sql.str());
    auto prepared = remove.Prepare();
    if (!prepared.ok()) return prepared;
    for (std::size_t i = 0; i < ids.size(); ++i) remove.BindInt64(static_cast<int>(i + 1), ids[i]);
    auto ran = remove.Run();
    if (!ran.ok()) return ran;
    if (sqlite3_changes(db_) != static_cast<int>(ids.size())) {
        return Result<void>::Failure({ErrorCode::Conflict, "部分历史记录已发生变化，请刷新后重试"});
    }
    return Result<void>::Success();
}

Result<HistoryEvent> SqliteHistoryRepository::GetLatestForTask(std::int64_t taskId) {
    if (db_ == nullptr) return Result<HistoryEvent>::Failure(InternalSqliteError());
    SqliteStatement query(db_, std::string("SELECT ") + kColumns + " FROM history WHERE task_id=? ORDER BY event_at DESC,id DESC LIMIT 1;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<HistoryEvent>::Failure(prepared.error());
    query.BindInt64(1, taskId);
    auto row = query.Step();
    if (!row.ok()) return Result<HistoryEvent>::Failure(row.error());
    if (!row.value()) return Result<HistoryEvent>::Failure({ErrorCode::NotFound, "历史记录不存在"});
    auto event = ReadEvent(query);
    if (!event) return Result<HistoryEvent>::Failure({ErrorCode::QueryFailed, "历史记录已损坏"});
    return Result<HistoryEvent>::Success(*event);
}

}  // namespace todo
