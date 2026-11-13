#include "data/MySqlHistoryRepository.h"

#include <functional>
#include <limits>
#include <memory>
#include <sstream>

#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <optional>
#include <string>
#include <vector>

#include "data/MySqlErrors.h"
#include "domain/HistoryPagination.h"
#include "domain/TaskRules.h"

namespace todo {
namespace {
constexpr const char* kColumns = "id,task_id,title,note,priority,due_time,original_category_id,original_category_name,original_category_color,task_created_at,completed_at,event_at,operation_type";
void BindTime(sql::PreparedStatement* statement, int index, const std::optional<LocalDateTime>& value) {
    if (value) statement->setString(index, value->ToString()); else statement->setNull(index, 0);
}
void BindId(sql::PreparedStatement* statement, int index, std::optional<std::int64_t> value) {
    if (value) statement->setInt64(index, *value); else statement->setNull(index, 0);
}
std::optional<LocalDateTime> OptionalTime(sql::ResultSet& row, const char* name) {
    if (row.isNull(name)) return std::nullopt;
    return LocalDateTime::Parse(row.getString(name).c_str());
}
std::optional<HistoryEvent> ReadEvent(sql::ResultSet& row) {
    const auto id = row.getUInt64("id");
    const auto taskId = row.getUInt64("task_id");
    if (id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || taskId > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    auto priority = PriorityFromString(row.getString("priority").c_str());
    auto operation = OperationTypeFromString(row.getString("operation_type").c_str());
    auto created = LocalDateTime::Parse(row.getString("task_created_at").c_str());
    auto eventAt = LocalDateTime::Parse(row.getString("event_at").c_str());
    if (!priority || !operation || !created || !eventAt) return std::nullopt;
    HistoryEvent event;
    event.id = static_cast<std::int64_t>(id);
    event.taskId = static_cast<std::int64_t>(taskId);
    event.snapshot.title = row.getString("title").c_str();
    event.snapshot.note = row.getString("note").c_str();
    event.snapshot.priority = *priority;
    event.snapshot.dueAt = OptionalTime(row, "due_time");
    event.snapshot.taskCreatedAt = *created;
    event.completedAt = OptionalTime(row, "completed_at");
    event.eventAt = *eventAt;
    event.operationType = *operation;
    if (!row.isNull("original_category_id")) event.snapshot.originalCategoryId = row.getInt64("original_category_id");
    if (!row.isNull("original_category_name")) event.snapshot.originalCategoryName = row.getString("original_category_name").c_str();
    if (!row.isNull("original_category_color")) event.snapshot.originalCategoryColor = row.getString("original_category_color").c_str();
    return event;
}
}

MySqlHistoryRepository::MySqlHistoryRepository(sql::Connection* connection) : connection_(connection) {}

Result<HistoryEvent> MySqlHistoryRepository::Append(const HistoryEvent& event) {
    try {
        std::unique_ptr<sql::PreparedStatement> insert(connection_->prepareStatement("INSERT INTO history(task_id,title,note,priority,due_time,original_category_id,original_category_name,original_category_color,task_created_at,completed_at,event_at,operation_type) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"));
        insert->setInt64(1, event.taskId);
        insert->setString(2, event.snapshot.title);
        insert->setString(3, event.snapshot.note);
        insert->setString(4, ToString(event.snapshot.priority));
        BindTime(insert.get(), 5, event.snapshot.dueAt);
        BindId(insert.get(), 6, event.snapshot.originalCategoryId);
        if (event.snapshot.originalCategoryName.empty()) insert->setNull(7, 0); else insert->setString(7, event.snapshot.originalCategoryName);
        if (event.snapshot.originalCategoryColor.empty()) insert->setNull(8, 0); else insert->setString(8, event.snapshot.originalCategoryColor);
        insert->setString(9, event.snapshot.taskCreatedAt.ToString());
        BindTime(insert.get(), 10, event.completedAt);
        insert->setString(11, event.eventAt.ToString());
        insert->setString(12, ToString(event.operationType));
        insert->executeUpdate();
        std::unique_ptr<sql::Statement> idQuery(connection_->createStatement());
        std::unique_ptr<sql::ResultSet> id(idQuery->executeQuery("SELECT LAST_INSERT_ID()"));
        if (!id->next()) return Result<HistoryEvent>::Failure(InternalDataError());
        HistoryEvent stored = event;
        stored.id = static_cast<std::int64_t>(id->getUInt64(1));
        return Result<HistoryEvent>::Success(std::move(stored));
    } catch (const sql::SQLException& error) { return Result<HistoryEvent>::Failure(TranslateMySqlError(error)); }
}

Result<HistoryPage> MySqlHistoryRepository::QueryPage(const HistoryQuery& request) {
    try {
        const std::string signature = HistoryPagination::QuerySignature(request);
        if (!HistoryPagination::IsCursorCompatible(request)) return Result<HistoryPage>::Failure({ErrorCode::Conflict, "历史筛选条件已变化"});
        std::ostringstream sql;
        sql << "SELECT " << kColumns << " FROM history WHERE 1=1";
        std::vector<std::function<void(sql::PreparedStatement*, int&)>> binders;
        if (request.filter.categoryId) {
            sql << " AND original_category_id=?";
            binders.push_back([id = *request.filter.categoryId](auto* s, int& i) { s->setInt64(i++, id); });
        }
        if (request.filter.timeRange) {
            sql << " AND event_at>=? AND event_at<?";
            const auto range = *request.filter.timeRange;
            binders.push_back([range](auto* s, int& i) { s->setString(i++, range.start.ToString()); s->setString(i++, range.end.ToString()); });
        }
        if (!request.filter.keyword.empty()) {
            sql << " AND (title LIKE ? ESCAPE '\\\\' OR note LIKE ? ESCAPE '\\\\')";
            const std::string pattern = "%" + TaskRules::EscapeLike(request.filter.keyword) + "%";
            binders.push_back([pattern](auto* s, int& i) { s->setString(i++, pattern); s->setString(i++, pattern); });
        }
        if (request.page.cursor) {
            const auto cursor = *request.page.cursor;
            sql << " AND (event_at<? OR (event_at=? AND id<?))";
            binders.push_back([cursor](auto* s, int& i) { s->setString(i++, cursor.eventAt.ToString()); s->setString(i++, cursor.eventAt.ToString()); s->setInt64(i++, cursor.id); });
        }
        sql << " ORDER BY event_at DESC,id DESC LIMIT ?";
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement(sql.str()));
        int index = 1;
        for (auto& bind : binders) bind(query.get(), index);
        query->setInt(index, request.page.pageSize + 1);
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        HistoryPage page;
        page.signature = signature;
        while (rows->next()) { auto event = ReadEvent(*rows); if (event) page.items.push_back(std::move(*event)); }
        if (static_cast<int>(page.items.size()) > request.page.pageSize) {
            page.items.resize(request.page.pageSize);
            const auto& last = page.items.back();
            page.nextCursor = HistoryPagination::MakeNextHistoryCursor(last, request, signature);
        }
        return Result<HistoryPage>::Success(std::move(page));
    } catch (const sql::SQLException& error) { return Result<HistoryPage>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlHistoryRepository::DeleteEvents(const std::vector<std::int64_t>& ids) {
    try {
        std::ostringstream sql;
        sql << "DELETE FROM history WHERE id IN (";
        for (std::size_t i = 0; i < ids.size(); ++i) { if (i) sql << ','; sql << '?'; }
        sql << ')';
        std::unique_ptr<sql::PreparedStatement> remove(connection_->prepareStatement(sql.str()));
        for (std::size_t i = 0; i < ids.size(); ++i) remove->setInt64(static_cast<int>(i + 1), ids[i]);
        const auto count = remove->executeUpdate();
        if (count != static_cast<int>(ids.size())) return Result<void>::Failure({ErrorCode::Conflict, "部分历史记录已发生变化，请刷新后重试"});
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

Result<HistoryEvent> MySqlHistoryRepository::GetLatestForTask(std::int64_t taskId) {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement(std::string("SELECT ") + kColumns + " FROM history WHERE task_id=? ORDER BY event_at DESC,id DESC LIMIT 1"));
        query->setInt64(1, taskId);
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        if (!rows->next()) return Result<HistoryEvent>::Failure({ErrorCode::NotFound, "历史记录不存在"});
        auto event = ReadEvent(*rows);
        if (!event) return Result<HistoryEvent>::Failure({ErrorCode::QueryFailed, "历史记录已损坏"});
        return Result<HistoryEvent>::Success(*event);
    } catch (const sql::SQLException& error) { return Result<HistoryEvent>::Failure(TranslateMySqlError(error)); }
}

}  // namespace todo
