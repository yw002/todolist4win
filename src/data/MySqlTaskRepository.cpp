#include "data/MySqlTaskRepository.h"

#include <cmath>
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
#include "domain/SortWeight.h"
#include "domain/TaskPagination.h"
#include "domain/TaskRules.h"
#include "domain/TimeRanges.h"

namespace todo {
namespace {
constexpr const char* kColumns = "id,category_id,title,note,priority,due_time,completed,created_at,completed_at,sort_weight,revision";

void BindOptionalTime(sql::PreparedStatement* statement, int index, const std::optional<LocalDateTime>& value) {
    if (value) statement->setString(index, value->ToString()); else statement->setNull(index, 0);
}
void BindOptionalId(sql::PreparedStatement* statement, int index, std::optional<std::int64_t> value) {
    if (value) statement->setInt64(index, *value); else statement->setNull(index, 0);
}
std::optional<LocalDateTime> ReadOptionalTime(sql::ResultSet& row, const char* name) {
    if (row.isNull(name)) return std::nullopt;
    return LocalDateTime::Parse(row.getString(name).c_str());
}
std::optional<Task> ReadTask(sql::ResultSet& row) {
    const auto rawId = row.getUInt64("id");
    if (rawId > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    auto priority = PriorityFromString(row.getString("priority").c_str());
    auto created = LocalDateTime::Parse(row.getString("created_at").c_str());
    if (!priority || !created) return std::nullopt;
    Task task;
    task.id = static_cast<std::int64_t>(rawId);
    if (!row.isNull("category_id")) {
        const auto category = row.getUInt64("category_id");
        if (category > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        task.categoryId = static_cast<std::int64_t>(category);
    }
    task.title = row.getString("title").c_str();
    task.note = row.getString("note").c_str();
    task.priority = *priority;
    task.dueAt = ReadOptionalTime(row, "due_time");
    if (!row.isNull("due_time") && !task.dueAt) return std::nullopt;
    task.completed = row.getBoolean("completed");
    task.createdAt = *created;
    task.completedAt = ReadOptionalTime(row, "completed_at");
    if (task.completed != task.completedAt.has_value()) return std::nullopt;
    task.sortWeight = row.getDouble("sort_weight");
    task.revision = row.getUInt64("revision");
    if (!TaskRules::ValidateTask(task).ok()) return std::nullopt;
    return task;
}
void AppendFilter(std::ostringstream& sql, std::vector<std::function<void(sql::PreparedStatement*, int&)>>& binders, const TaskQuery& query) {
    if (query.route == Route::Category) {
        sql << " AND category_id=?";
        binders.push_back([id = query.categoryId.value_or(0)](auto* s, int& i) { s->setInt64(i++, id); });
    } else if (query.route == Route::Today) {
        sql << " AND completed=0 AND due_time>=? AND due_time<?";
        const auto range = TimeRanges::Today(query.now);
        binders.push_back([range](auto* s, int& i) { s->setString(i++, range.start.ToString()); s->setString(i++, range.end.ToString()); });
    } else if (query.route == Route::Overdue) {
        sql << " AND completed=0 AND due_time IS NOT NULL AND due_time<?";
        binders.push_back([now = query.now](auto* s, int& i) { s->setString(i++, now.ToString()); });
    }
    if (query.route == Route::AllTasks || query.route == Route::Category) {
        if (query.display == CompletionDisplay::Archive) sql << " AND completed=0";
        if (query.filter.completion == CompletionFilter::Pending) sql << " AND completed=0";
        else if (query.filter.completion == CompletionFilter::Completed) sql << " AND completed=1";
    }
    if (query.filter.priority) {
        sql << " AND priority=?";
        binders.push_back([priority = *query.filter.priority](auto* s, int& i) { s->setString(i++, ToString(priority)); });
    }
    if (!query.filter.keyword.empty()) {
        sql << " AND (title LIKE ? ESCAPE '\\\\' OR note LIKE ? ESCAPE '\\\\')";
        const std::string pattern = "%" + TaskRules::EscapeLike(query.filter.keyword) + "%";
        binders.push_back([pattern](auto* s, int& i) { s->setString(i++, pattern); s->setString(i++, pattern); });
    }
    if (query.filter.dueRange) {
        sql << " AND due_time>=? AND due_time<?";
        const auto range = *query.filter.dueRange;
        binders.push_back([range](auto* s, int& i) { s->setString(i++, range.start.ToString()); s->setString(i++, range.end.ToString()); });
    }
}
}

MySqlTaskRepository::MySqlTaskRepository(sql::Connection* connection) : connection_(connection) {}

Result<Task> MySqlTaskRepository::Create(const Task& task) {
    try {
        double maximum = 0;
        std::unique_ptr<sql::PreparedStatement> last(connection_->prepareStatement("SELECT sort_weight FROM task ORDER BY sort_weight DESC,id DESC LIMIT 1 FOR UPDATE"));
        std::unique_ptr<sql::ResultSet> lastRow(last->executeQuery());
        if (lastRow->next()) maximum = lastRow->getDouble(1);
        lastRow.reset();
        last.reset();
        if (!std::isfinite(maximum) || !std::isfinite(SortWeight::NewTaskWeight(maximum)) || SortWeight::NewTaskWeight(maximum) <= maximum) {
            auto normalized = NormalizeWeights();
            if (!normalized.ok()) return Result<Task>::Failure(normalized.error());
            last.reset(connection_->prepareStatement("SELECT COALESCE(MAX(sort_weight),0) FROM task"));
            lastRow.reset(last->executeQuery());
            if (lastRow->next()) maximum = lastRow->getDouble(1);
            lastRow.reset();
            last.reset();
        }
        std::unique_ptr<sql::PreparedStatement> insert(connection_->prepareStatement("INSERT INTO task(category_id,title,note,priority,due_time,completed,created_at,completed_at,sort_weight,revision) VALUES (?,?,?,?,?,0,?,NULL,?,1)"));
        BindOptionalId(insert.get(), 1, task.categoryId);
        insert->setString(2, TaskRules::Trim(task.title));
        insert->setString(3, task.note);
        insert->setString(4, ToString(task.priority));
        BindOptionalTime(insert.get(), 5, task.dueAt);
        insert->setString(6, task.createdAt.ToString());
        insert->setDouble(7, maximum == 0.0 ? SortWeight::kStep : SortWeight::NewTaskWeight(maximum));
        insert->executeUpdate();
        std::unique_ptr<sql::Statement> idQuery(connection_->createStatement());
        std::unique_ptr<sql::ResultSet> id(idQuery->executeQuery("SELECT LAST_INSERT_ID()"));
        if (!id->next()) return Result<Task>::Failure(InternalDataError());
        return GetByIdForUpdate(static_cast<std::int64_t>(id->getUInt64(1)));
    } catch (const sql::SQLException& error) { return Result<Task>::Failure(TranslateMySqlError(error)); }
}

Result<Task> MySqlTaskRepository::Update(const Task& task, std::uint64_t expectedRevision) {
    try {
        std::unique_ptr<sql::PreparedStatement> update(connection_->prepareStatement("UPDATE task SET category_id=?,title=?,note=?,priority=?,due_time=?,completed=?,created_at=?,completed_at=?,sort_weight=?,revision=revision+1 WHERE id=? AND revision=?"));
        BindOptionalId(update.get(), 1, task.categoryId);
        update->setString(2, TaskRules::Trim(task.title));
        update->setString(3, task.note);
        update->setString(4, ToString(task.priority));
        BindOptionalTime(update.get(), 5, task.dueAt);
        update->setBoolean(6, task.completed);
        update->setString(7, task.createdAt.ToString());
        BindOptionalTime(update.get(), 8, task.completedAt);
        update->setDouble(9, task.sortWeight);
        update->setInt64(10, task.id);
        update->setUInt64(11, expectedRevision);
        if (update->executeUpdate() != 1) return Result<Task>::Failure({ErrorCode::Conflict, "任务不存在或已被修改"});
        return GetByIdForUpdate(task.id);
    } catch (const sql::SQLException& error) { return Result<Task>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlTaskRepository::DeleteTask(std::int64_t id) {
    try {
        std::unique_ptr<sql::PreparedStatement> remove(connection_->prepareStatement("DELETE FROM task WHERE id=?"));
        remove->setInt64(1, id);
        if (remove->executeUpdate() != 1) return Result<void>::Failure({ErrorCode::NotFound, "任务不存在"});
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

Result<Task> MySqlTaskRepository::GetByIdForUpdate(std::int64_t id) {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement(std::string("SELECT ") + kColumns + " FROM task WHERE id=? FOR UPDATE"));
        query->setInt64(1, id);
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        if (!rows->next()) return Result<Task>::Failure({ErrorCode::NotFound, "任务不存在"});
        auto task = ReadTask(*rows);
        if (!task) return Result<Task>::Failure({ErrorCode::QueryFailed, "任务记录已损坏"});
        return Result<Task>::Success(*task);
    } catch (const sql::SQLException& error) { return Result<Task>::Failure(TranslateMySqlError(error)); }
}

Result<TaskPage> MySqlTaskRepository::QueryPage(const TaskQuery& request) {
    try {
        const std::string signature = TaskPagination::TaskQuerySignature(request);
        if (!TaskPagination::IsTaskCursorCompatible(request)) return Result<TaskPage>::Failure({ErrorCode::Conflict, "查询条件已变化，请从第一页加载"});
        std::ostringstream sql;
        sql << "SELECT " << kColumns << " FROM task WHERE 1=1";
        std::vector<std::function<void(sql::PreparedStatement*, int&)>> binders;
        AppendFilter(sql, binders, request);
        const bool dueOrder = request.route == Route::Today || request.route == Route::Overdue;
        if (request.page.cursor) {
            const auto cursor = *request.page.cursor;
            if (dueOrder && cursor.dueAt) {
                sql << " AND (due_time>? OR (due_time=? AND sort_weight>?) OR (due_time=? AND sort_weight=? AND id>?))";
                binders.push_back([cursor](auto* s, int& i) {
                    s->setString(i++, cursor.dueAt->ToString()); s->setString(i++, cursor.dueAt->ToString()); s->setDouble(i++, cursor.sortWeight);
                    s->setString(i++, cursor.dueAt->ToString()); s->setDouble(i++, cursor.sortWeight); s->setInt64(i++, cursor.id);
                });
            } else if (!dueOrder) {
                sql << " AND (sort_weight>? OR (sort_weight=? AND id>?))";
                binders.push_back([cursor](auto* s, int& i) { s->setDouble(i++, cursor.sortWeight); s->setDouble(i++, cursor.sortWeight); s->setInt64(i++, cursor.id); });
            }
        }
        sql << (dueOrder ? " ORDER BY due_time,sort_weight,id" : " ORDER BY sort_weight,id") << " LIMIT ?";
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement(sql.str()));
        int index = 1;
        for (auto& bind : binders) bind(query.get(), index);
        query->setInt(index, request.page.pageSize + 1);
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        TaskPage page;
        page.signature = signature;
        while (rows->next()) {
            auto task = ReadTask(*rows);
            if (task) page.items.push_back(std::move(*task));
        }
        if (static_cast<int>(page.items.size()) > request.page.pageSize) {
            page.items.resize(request.page.pageSize);
            const auto& last = page.items.back();
            page.nextCursor = TaskPagination::MakeNextTaskCursor(last, request, signature);
        }
        return Result<TaskPage>::Success(std::move(page));
    } catch (const sql::SQLException& error) { return Result<TaskPage>::Failure(TranslateMySqlError(error)); }
}

Result<double> MySqlTaskRepository::WeightOf(std::int64_t id) {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement("SELECT sort_weight FROM task WHERE id=? FOR UPDATE"));
        query->setInt64(1, id);
        std::unique_ptr<sql::ResultSet> row(query->executeQuery());
        if (!row->next()) return Result<double>::Failure({ErrorCode::NotFound, "排序邻居已不存在"});
        return Result<double>::Success(row->getDouble(1));
    } catch (const sql::SQLException& error) { return Result<double>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlTaskRepository::NormalizeWeights() {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement("SELECT id FROM task ORDER BY sort_weight,id FOR UPDATE"));
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        std::vector<std::int64_t> ids;
        while (rows->next()) ids.push_back(rows->getInt64(1));
        std::unique_ptr<sql::PreparedStatement> update(connection_->prepareStatement("UPDATE task SET sort_weight=?,revision=revision+1 WHERE id=?"));
        for (std::size_t i = 0; i < ids.size(); ++i) {
            update->setDouble(1, static_cast<double>(i + 1) * SortWeight::kStep);
            update->setInt64(2, ids[i]);
            update->executeUpdate();
        }
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlTaskRepository::Reorder(const ReorderRequest& request) {
    auto moved = GetByIdForUpdate(request.taskId);
    if (!moved.ok()) return Result<void>::Failure(moved.error());
    std::optional<double> previous;
    std::optional<double> next;
    if (request.previousId) { auto value = WeightOf(*request.previousId); if (!value.ok()) return Result<void>::Failure(value.error()); previous = value.value(); }
    if (request.nextId) { auto value = WeightOf(*request.nextId); if (!value.ok()) return Result<void>::Failure(value.error()); next = value.value(); }
    auto weight = SortWeight::Between(previous, next);
    if (!weight.ok() && weight.error().code == ErrorCode::Conflict) {
        auto normalized = NormalizeWeights();
        if (!normalized.ok()) return normalized;
        if (request.previousId) { auto value = WeightOf(*request.previousId); if (!value.ok()) return Result<void>::Failure(value.error()); previous = value.value(); }
        if (request.nextId) { auto value = WeightOf(*request.nextId); if (!value.ok()) return Result<void>::Failure(value.error()); next = value.value(); }
        weight = SortWeight::Between(previous, next);
    }
    if (!weight.ok()) return Result<void>::Failure(weight.error());
    try {
        std::unique_ptr<sql::PreparedStatement> update(connection_->prepareStatement("UPDATE task SET sort_weight=?,revision=revision+1 WHERE id=?"));
        update->setDouble(1, weight.value());
        update->setInt64(2, request.taskId);
        if (update->executeUpdate() != 1) return Result<void>::Failure({ErrorCode::Conflict, "任务排序冲突"});
        return Result<void>::Success();
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

}  // namespace todo
