#include "data/SqliteCategoryRepository.h"

#include <optional>
#include <string>
#include <utility>

#include <sqlite3.h>

#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"
#include "domain/TaskRules.h"

namespace todo {
namespace {

std::optional<Category> ReadCategory(SqliteStatement& row) {
    const std::int64_t id = row.ColumnInt64(0);
    if (id <= 0) return std::nullopt;
    auto created = LocalDateTime::Parse(row.ColumnText(3));
    if (!created) return std::nullopt;
    Category category;
    category.id = id;
    category.name = row.ColumnText(1);
    category.color = row.ColumnText(2);
    category.createdAt = *created;
    if (!TaskRules::ValidateCategoryName(category.name).ok() || !TaskRules::ValidateColor(category.color).ok()) return std::nullopt;
    return category;
}

}  // namespace

SqliteCategoryRepository::SqliteCategoryRepository(sqlite3* db) : db_(db) {}

Result<Category> SqliteCategoryRepository::Create(const Category& category) {
    if (db_ == nullptr) return Result<Category>::Failure(InternalSqliteError());
    SqliteStatement insert(db_, "INSERT INTO category(name,color,created_at) VALUES (?,?,?);");
    auto prepared = insert.Prepare();
    if (!prepared.ok()) return Result<Category>::Failure(prepared.error());
    insert.BindText(1, category.name);
    insert.BindText(2, category.color);
    insert.BindText(3, category.createdAt.ToString());
    auto ran = insert.Run();
    if (!ran.ok()) return Result<Category>::Failure(ran.error());
    return GetById(static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_)));
}

Result<Category> SqliteCategoryRepository::Update(const Category& category) {
    if (db_ == nullptr) return Result<Category>::Failure(InternalSqliteError());
    SqliteStatement update(db_, "UPDATE category SET name=?,color=? WHERE id=?;");
    auto prepared = update.Prepare();
    if (!prepared.ok()) return Result<Category>::Failure(prepared.error());
    update.BindText(1, category.name);
    update.BindText(2, category.color);
    update.BindInt64(3, category.id);
    auto ran = update.Run();
    if (!ran.ok()) return Result<Category>::Failure(ran.error());
    if (sqlite3_changes(db_) != 1) return Result<Category>::Failure({ErrorCode::NotFound, "分类不存在"});
    return GetById(category.id);
}

Result<void> SqliteCategoryRepository::DeleteIfEmpty(std::int64_t id) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    {
        SqliteStatement exists(db_, "SELECT id FROM category WHERE id=?;");
        auto prepared = exists.Prepare();
        if (!prepared.ok()) return prepared;
        exists.BindInt64(1, id);
        auto row = exists.Step();
        if (!row.ok()) return Result<void>::Failure(row.error());
        if (!row.value()) return Result<void>::Failure({ErrorCode::NotFound, "分类不存在"});
    }
    {
        SqliteStatement used(db_, "SELECT 1 FROM task WHERE category_id=? LIMIT 1;");
        auto prepared = used.Prepare();
        if (!prepared.ok()) return prepared;
        used.BindInt64(1, id);
        auto row = used.Step();
        if (!row.ok()) return Result<void>::Failure(row.error());
        if (row.value()) return Result<void>::Failure({ErrorCode::CategoryNotEmpty, "该分类仍有任务，请先迁移任务"});
    }
    SqliteStatement remove(db_, "DELETE FROM category WHERE id=?;");
    auto prepared = remove.Prepare();
    if (!prepared.ok()) return prepared;
    remove.BindInt64(1, id);
    auto ran = remove.Run();
    if (!ran.ok()) return ran;
    if (sqlite3_changes(db_) != 1) return Result<void>::Failure({ErrorCode::Conflict, "分类已发生变化"});
    return Result<void>::Success();
}

Result<std::vector<CategoryCount>> SqliteCategoryRepository::ListWithCounts() {
    if (db_ == nullptr) return Result<std::vector<CategoryCount>>::Failure(InternalSqliteError());
    SqliteStatement query(db_,
                          "SELECT c.id,c.name,c.color,c.created_at,COUNT(t.id) AS total,"
                          "COALESCE(SUM(CASE WHEN t.completed=0 THEN 1 ELSE 0 END),0) AS pending,"
                          "COALESCE(SUM(CASE WHEN t.completed=1 THEN 1 ELSE 0 END),0) AS completed "
                          "FROM category c LEFT JOIN task t ON t.category_id=c.id "
                          "GROUP BY c.id,c.name,c.color,c.created_at "
                          "ORDER BY c.name COLLATE NOCASE, c.id;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<std::vector<CategoryCount>>::Failure(prepared.error());
    std::vector<CategoryCount> result;
    for (;;) {
        auto row = query.Step();
        if (!row.ok()) return Result<std::vector<CategoryCount>>::Failure(row.error());
        if (!row.value()) break;
        auto category = ReadCategory(query);
        if (!category) continue;
        result.push_back(CategoryCount{*category, query.ColumnInt64(4), query.ColumnInt64(5), query.ColumnInt64(6)});
    }
    return Result<std::vector<CategoryCount>>::Success(std::move(result));
}

Result<Category> SqliteCategoryRepository::GetById(std::int64_t id) {
    if (db_ == nullptr) return Result<Category>::Failure(InternalSqliteError());
    SqliteStatement query(db_, "SELECT id,name,color,created_at FROM category WHERE id=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<Category>::Failure(prepared.error());
    query.BindInt64(1, id);
    auto row = query.Step();
    if (!row.ok()) return Result<Category>::Failure(row.error());
    if (!row.value()) return Result<Category>::Failure({ErrorCode::NotFound, "分类不存在"});
    auto category = ReadCategory(query);
    if (!category) return Result<Category>::Failure({ErrorCode::QueryFailed, "分类记录已损坏"});
    return Result<Category>::Success(*category);
}

}  // namespace todo
