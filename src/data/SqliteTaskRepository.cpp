#include "data/SqliteTaskRepository.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"
#include "domain/SortWeight.h"
#include "domain/TaskPagination.h"
#include "domain/TaskRules.h"
#include "domain/TimeRanges.h"

namespace todo {
namespace {

// body_doc is appended LAST so the existing 0..10 column indices in ReadTask stay untouched.
// It is a compact JSON string (<=256 KiB); task_image.data is never part of a list/paging SELECT.
constexpr const char* kColumns = "id,category_id,title,note,priority,due_time,completed,created_at,completed_at,sort_weight,revision,body_doc";

using Binder = std::function<void(SqliteStatement&, int&)>;

std::optional<LocalDateTime> ReadOptionalTime(const SqliteStatement& row, int index) {
    if (row.ColumnIsNull(index)) return std::nullopt;
    return LocalDateTime::Parse(row.ColumnText(index));
}

std::optional<Task> ReadTask(SqliteStatement& row) {
    const std::int64_t id = row.ColumnInt64(0);
    if (id <= 0) return std::nullopt;
    auto priority = PriorityFromString(row.ColumnText(4));
    auto created = LocalDateTime::Parse(row.ColumnText(7));
    if (!priority || !created) return std::nullopt;
    Task task;
    task.id = id;
    if (!row.ColumnIsNull(1)) task.categoryId = row.ColumnInt64(1);
    task.title = row.ColumnText(2);
    task.note = row.ColumnText(3);
    task.priority = *priority;
    task.dueAt = ReadOptionalTime(row, 5);
    if (!row.ColumnIsNull(5) && !task.dueAt) return std::nullopt;
    task.completed = row.ColumnInt64(6) != 0;
    task.createdAt = *created;
    task.completedAt = ReadOptionalTime(row, 8);
    if (task.completed != task.completedAt.has_value()) return std::nullopt;
    task.sortWeight = row.ColumnDouble(9);
    task.revision = static_cast<std::uint64_t>(row.ColumnInt64(10));
    // Verbatim, never parsed: a corrupt or newer-version document must not make this row unreadable.
    // An empty string is the in-memory representation of SQL NULL (a legacy note-only task).
    if (!row.ColumnIsNull(11)) task.bodyDoc = row.ColumnText(11);
    if (!TaskRules::ValidateTask(task).ok()) return std::nullopt;
    return task;
}

void AppendFilter(std::ostringstream& sql, std::vector<Binder>& binders, const TaskQuery& query) {
    if (query.route == Route::Category) {
        sql << " AND category_id=?";
        binders.push_back([id = query.categoryId.value_or(0)](SqliteStatement& s, int& i) { s.BindInt64(i++, id); });
    } else if (query.route == Route::Today) {
        sql << " AND completed=0 AND due_time>=? AND due_time<?";
        const auto range = TimeRanges::Today(query.now);
        binders.push_back([range](SqliteStatement& s, int& i) {
            s.BindText(i++, range.start.ToString());
            s.BindText(i++, range.end.ToString());
        });
    } else if (query.route == Route::Overdue) {
        sql << " AND completed=0 AND due_time IS NOT NULL AND due_time<?";
        // The upper bound is the frozen session time (query.now), the same value folded into the
        // signature, so a continuation page cannot drift across the overdue boundary.
        binders.push_back([now = query.now](SqliteStatement& s, int& i) { s.BindText(i++, now.ToString()); });
    }
    if (query.route == Route::AllTasks || query.route == Route::Category) {
        if (query.display == CompletionDisplay::Archive) sql << " AND completed=0";
        if (query.filter.completion == CompletionFilter::Pending) sql << " AND completed=0";
        else if (query.filter.completion == CompletionFilter::Completed) sql << " AND completed=1";
    }
    if (query.filter.priority) {
        sql << " AND priority=?";
        binders.push_back([priority = *query.filter.priority](SqliteStatement& s, int& i) { s.BindText(i++, ToString(priority)); });
    }
    if (!query.filter.keyword.empty()) {
        sql << " AND (title LIKE ? ESCAPE '\\' OR note LIKE ? ESCAPE '\\')";
        const std::string pattern = "%" + TaskRules::EscapeLike(query.filter.keyword) + "%";
        binders.push_back([pattern](SqliteStatement& s, int& i) {
            s.BindText(i++, pattern);
            s.BindText(i++, pattern);
        });
    }
    if (query.filter.dueRange) {
        sql << " AND due_time>=? AND due_time<?";
        const auto range = *query.filter.dueRange;
        binders.push_back([range](SqliteStatement& s, int& i) {
            s.BindText(i++, range.start.ToString());
            s.BindText(i++, range.end.ToString());
        });
    }
}

}  // namespace

SqliteTaskRepository::SqliteTaskRepository(sqlite3* db) : db_(db) {}

Result<Task> SqliteTaskRepository::Create(const Task& task) {
    if (db_ == nullptr) return Result<Task>::Failure(InternalSqliteError());
    double maximum = 0.0;
    {
        SqliteStatement last(db_, "SELECT sort_weight FROM task ORDER BY sort_weight DESC, id DESC LIMIT 1;");
        auto prepared = last.Prepare();
        if (!prepared.ok()) return Result<Task>::Failure(prepared.error());
        auto row = last.Step();
        if (!row.ok()) return Result<Task>::Failure(row.error());
        if (row.value()) maximum = last.ColumnDouble(0);
    }
    if (!std::isfinite(maximum) || !std::isfinite(SortWeight::NewTaskWeight(maximum)) || SortWeight::NewTaskWeight(maximum) <= maximum) {
        auto normalized = NormalizeWeights();
        if (!normalized.ok()) return Result<Task>::Failure(normalized.error());
        SqliteStatement last(db_, "SELECT COALESCE(MAX(sort_weight),0) FROM task;");
        auto prepared = last.Prepare();
        if (!prepared.ok()) return Result<Task>::Failure(prepared.error());
        auto row = last.Step();
        if (!row.ok()) return Result<Task>::Failure(row.error());
        if (row.value()) maximum = last.ColumnDouble(0);
    }
    SqliteStatement insert(db_,
                           "INSERT INTO task(category_id,title,note,priority,due_time,completed,created_at,completed_at,sort_weight,revision,body_doc)"
                           " VALUES (?,?,?,?,?,0,?,NULL,?,1,?);");
    auto prepared = insert.Prepare();
    if (!prepared.ok()) return Result<Task>::Failure(prepared.error());
    if (task.categoryId) insert.BindInt64(1, *task.categoryId); else insert.BindNull(1);
    insert.BindText(2, TaskRules::Trim(task.title));
    insert.BindText(3, task.note);
    insert.BindText(4, ToString(task.priority));
    if (task.dueAt) insert.BindText(5, task.dueAt->ToString()); else insert.BindNull(5);
    insert.BindText(6, task.createdAt.ToString());
    insert.BindDouble(7, maximum == 0.0 ? SortWeight::kStep : SortWeight::NewTaskWeight(maximum));
    // A fast-add command without a body keeps the legacy NULL: no bulk UPDATE of existing rows.
    if (task.bodyDoc.empty()) insert.BindNull(8); else insert.BindText(8, task.bodyDoc);
    auto ran = insert.Run();
    if (!ran.ok()) return Result<Task>::Failure(ran.error());
    return GetByIdForUpdate(static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_)));
}

Result<Task> SqliteTaskRepository::Update(const Task& task, std::uint64_t expectedRevision) {
    if (db_ == nullptr) return Result<Task>::Failure(InternalSqliteError());
    SqliteStatement update(db_,
                           "UPDATE task SET category_id=?,title=?,note=?,priority=?,due_time=?,completed=?,created_at=?,completed_at=?,"
                           "sort_weight=?,body_doc=?,revision=revision+1 WHERE id=? AND revision=?;");
    auto prepared = update.Prepare();
    if (!prepared.ok()) return Result<Task>::Failure(prepared.error());
    if (task.categoryId) update.BindInt64(1, *task.categoryId); else update.BindNull(1);
    update.BindText(2, TaskRules::Trim(task.title));
    update.BindText(3, task.note);
    update.BindText(4, ToString(task.priority));
    if (task.dueAt) update.BindText(5, task.dueAt->ToString()); else update.BindNull(5);
    update.BindInt64(6, task.completed ? 1 : 0);
    update.BindText(7, task.createdAt.ToString());
    if (task.completedAt) update.BindText(8, task.completedAt->ToString()); else update.BindNull(8);
    update.BindDouble(9, task.sortWeight);
    // Whatever the caller loaded is written back verbatim, including a v>1 document it could not
    // parse. TaskService is the only place that derives/replaces body_doc.
    if (task.bodyDoc.empty()) update.BindNull(10); else update.BindText(10, task.bodyDoc);
    update.BindInt64(11, task.id);
    update.BindInt64(12, static_cast<std::int64_t>(expectedRevision));
    auto ran = update.Run();
    if (!ran.ok()) return Result<Task>::Failure(ran.error());
    if (sqlite3_changes(db_) != 1) return Result<Task>::Failure({ErrorCode::Conflict, "任务不存在或已被修改"});
    return GetByIdForUpdate(task.id);
}

Result<void> SqliteTaskRepository::DeleteTask(std::int64_t id) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    SqliteStatement remove(db_, "DELETE FROM task WHERE id=?;");
    auto prepared = remove.Prepare();
    if (!prepared.ok()) return prepared;
    remove.BindInt64(1, id);
    auto ran = remove.Run();
    if (!ran.ok()) return ran;
    if (sqlite3_changes(db_) != 1) return Result<void>::Failure({ErrorCode::NotFound, "任务不存在"});
    return Result<void>::Success();
}

Result<Task> SqliteTaskRepository::GetByIdForUpdate(std::int64_t id) {
    if (db_ == nullptr) return Result<Task>::Failure(InternalSqliteError());
    SqliteStatement query(db_, std::string("SELECT ") + kColumns + " FROM task WHERE id=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<Task>::Failure(prepared.error());
    query.BindInt64(1, id);
    auto row = query.Step();
    if (!row.ok()) return Result<Task>::Failure(row.error());
    if (!row.value()) return Result<Task>::Failure({ErrorCode::NotFound, "任务不存在"});
    auto task = ReadTask(query);
    if (!task) return Result<Task>::Failure({ErrorCode::QueryFailed, "任务记录已损坏"});
    return Result<Task>::Success(*task);
}

Result<TaskPage> SqliteTaskRepository::QueryPage(const TaskQuery& request) {
    if (db_ == nullptr) return Result<TaskPage>::Failure(InternalSqliteError());
    const std::string signature = TaskPagination::TaskQuerySignature(request);
    if (!TaskPagination::IsTaskCursorCompatible(request)) {
        return Result<TaskPage>::Failure({ErrorCode::Conflict, "查询条件已变化，请从第一页加载"});
    }
    std::ostringstream sql;
    sql << "SELECT " << kColumns << " FROM task WHERE 1=1";
    std::vector<Binder> binders;
    AppendFilter(sql, binders, request);
    const bool dueOrder = request.route == Route::Today || request.route == Route::Overdue;
    if (request.page.cursor) {
        const auto cursor = *request.page.cursor;
        if (dueOrder && cursor.dueAt) {
            sql << " AND (due_time>? OR (due_time=? AND sort_weight>?) OR (due_time=? AND sort_weight=? AND id>?))";
            binders.push_back([cursor](SqliteStatement& s, int& i) {
                s.BindText(i++, cursor.dueAt->ToString());
                s.BindText(i++, cursor.dueAt->ToString()); s.BindDouble(i++, cursor.sortWeight);
                s.BindText(i++, cursor.dueAt->ToString()); s.BindDouble(i++, cursor.sortWeight); s.BindInt64(i++, cursor.id);
            });
        } else if (!dueOrder) {
            sql << " AND (sort_weight>? OR (sort_weight=? AND id>?))";
            binders.push_back([cursor](SqliteStatement& s, int& i) {
                s.BindDouble(i++, cursor.sortWeight);
                s.BindDouble(i++, cursor.sortWeight);
                s.BindInt64(i++, cursor.id);
            });
        }
    }
    sql << (dueOrder ? " ORDER BY due_time,sort_weight,id" : " ORDER BY sort_weight,id") << " LIMIT ?;";
    SqliteStatement query(db_, sql.str());
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<TaskPage>::Failure(prepared.error());
    int index = 1;
    for (auto& bind : binders) bind(query, index);
    query.BindInt64(index, request.page.pageSize + 1);
    TaskPage page;
    page.signature = signature;
    for (;;) {
        auto row = query.Step();
        if (!row.ok()) return Result<TaskPage>::Failure(row.error());
        if (!row.value()) break;
        auto task = ReadTask(query);
        if (task) page.items.push_back(std::move(*task));
    }
    if (static_cast<int>(page.items.size()) > request.page.pageSize) {
        page.items.resize(static_cast<std::size_t>(request.page.pageSize));
        page.nextCursor = TaskPagination::MakeNextTaskCursor(page.items.back(), request, signature);
    }
    return Result<TaskPage>::Success(std::move(page));
}

Result<double> SqliteTaskRepository::WeightOf(std::int64_t id) {
    if (db_ == nullptr) return Result<double>::Failure(InternalSqliteError());
    SqliteStatement query(db_, "SELECT sort_weight FROM task WHERE id=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<double>::Failure(prepared.error());
    query.BindInt64(1, id);
    auto row = query.Step();
    if (!row.ok()) return Result<double>::Failure(row.error());
    if (!row.value()) return Result<double>::Failure({ErrorCode::NotFound, "排序邻居已不存在"});
    return Result<double>::Success(query.ColumnDouble(0));
}

Result<void> SqliteTaskRepository::NormalizeWeights() {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    std::vector<std::int64_t> ids;
    {
        SqliteStatement query(db_, "SELECT id FROM task ORDER BY sort_weight,id;");
        auto prepared = query.Prepare();
        if (!prepared.ok()) return prepared;
        for (;;) {
            auto row = query.Step();
            if (!row.ok()) return Result<void>::Failure(row.error());
            if (!row.value()) break;
            ids.push_back(query.ColumnInt64(0));
        }
    }
    SqliteStatement update(db_, "UPDATE task SET sort_weight=?,revision=revision+1 WHERE id=?;");
    auto prepared = update.Prepare();
    if (!prepared.ok()) return prepared;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        update.BindDouble(1, static_cast<double>(i + 1) * SortWeight::kStep);
        update.BindInt64(2, ids[i]);
        auto ran = update.Run();
        if (!ran.ok()) return ran;
        update.Reset();
    }
    return Result<void>::Success();
}

Result<void> SqliteTaskRepository::Reorder(const ReorderRequest& request) {
    auto moved = GetByIdForUpdate(request.taskId);
    if (!moved.ok()) return Result<void>::Failure(moved.error());
    std::optional<double> previous;
    std::optional<double> next;
    if (request.previousId) {
        auto value = WeightOf(*request.previousId);
        if (!value.ok()) return Result<void>::Failure(value.error());
        previous = value.value();
    }
    if (request.nextId) {
        auto value = WeightOf(*request.nextId);
        if (!value.ok()) return Result<void>::Failure(value.error());
        next = value.value();
    }
    auto weight = SortWeight::Between(previous, next);
    if (!weight.ok() && weight.error().code == ErrorCode::Conflict) {
        auto normalized = NormalizeWeights();
        if (!normalized.ok()) return normalized;
        if (request.previousId) {
            auto value = WeightOf(*request.previousId);
            if (!value.ok()) return Result<void>::Failure(value.error());
            previous = value.value();
        }
        if (request.nextId) {
            auto value = WeightOf(*request.nextId);
            if (!value.ok()) return Result<void>::Failure(value.error());
            next = value.value();
        }
        weight = SortWeight::Between(previous, next);
    }
    if (!weight.ok()) return Result<void>::Failure(weight.error());
    SqliteStatement update(db_, "UPDATE task SET sort_weight=?,revision=revision+1 WHERE id=?;");
    auto prepared = update.Prepare();
    if (!prepared.ok()) return prepared;
    update.BindDouble(1, weight.value());
    update.BindInt64(2, request.taskId);
    auto ran = update.Run();
    if (!ran.ok()) return ran;
    if (sqlite3_changes(db_) != 1) return Result<void>::Failure({ErrorCode::Conflict, "任务排序冲突"});
    return Result<void>::Success();
}

}  // namespace todo
