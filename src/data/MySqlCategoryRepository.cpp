#include "data/MySqlCategoryRepository.h"

#include <limits>
#include <memory>

#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <optional>
#include <vector>

#include "data/MySqlErrors.h"
#include "domain/TaskRules.h"

namespace todo {
namespace {
std::optional<Category> ReadCategory(sql::ResultSet& row) {
    const auto unsignedId = row.getUInt64("id");
    if (unsignedId > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    auto created = LocalDateTime::Parse(row.getString("created_at").c_str());
    if (!created) return std::nullopt;
    Category category;
    category.id = static_cast<std::int64_t>(unsignedId);
    category.name = row.getString("name").c_str();
    category.color = row.getString("color").c_str();
    category.createdAt = *created;
    if (!TaskRules::ValidateCategoryName(category.name).ok() || !TaskRules::ValidateColor(category.color).ok()) return std::nullopt;
    return category;
}
}

MySqlCategoryRepository::MySqlCategoryRepository(sql::Connection* connection) : connection_(connection) {}

Result<Category> MySqlCategoryRepository::Create(const Category& category) {
    try {
        std::unique_ptr<sql::PreparedStatement> insert(connection_->prepareStatement("INSERT INTO category(name,color,created_at) VALUES (?,?,?)"));
        insert->setString(1, category.name);
        insert->setString(2, category.color);
        insert->setString(3, category.createdAt.ToString());
        insert->executeUpdate();
        std::unique_ptr<sql::Statement> idQuery(connection_->createStatement());
        std::unique_ptr<sql::ResultSet> id(idQuery->executeQuery("SELECT LAST_INSERT_ID()"));
        if (!id->next()) return Result<Category>::Failure(InternalDataError());
        return GetById(static_cast<std::int64_t>(id->getUInt64(1)));
    } catch (const sql::SQLException& error) { return Result<Category>::Failure(TranslateMySqlError(error)); }
}

Result<Category> MySqlCategoryRepository::Update(const Category& category) {
    try {
        std::unique_ptr<sql::PreparedStatement> update(connection_->prepareStatement("UPDATE category SET name=?,color=? WHERE id=?"));
        update->setString(1, category.name);
        update->setString(2, category.color);
        update->setInt64(3, category.id);
        if (update->executeUpdate() != 1) return Result<Category>::Failure({ErrorCode::NotFound, "分类不存在"});
        return GetById(category.id);
    } catch (const sql::SQLException& error) { return Result<Category>::Failure(TranslateMySqlError(error)); }
}

Result<void> MySqlCategoryRepository::DeleteIfEmpty(std::int64_t id) {
    try {
        std::unique_ptr<sql::PreparedStatement> lock(connection_->prepareStatement("SELECT id FROM category WHERE id=? FOR UPDATE"));
        lock->setInt64(1, id);
        std::unique_ptr<sql::ResultSet> found(lock->executeQuery());
        if (!found->next()) return Result<void>::Failure({ErrorCode::NotFound, "分类不存在"});
        std::unique_ptr<sql::PreparedStatement> used(connection_->prepareStatement("SELECT id FROM task WHERE category_id=? LIMIT 1 FOR UPDATE"));
        used->setInt64(1, id);
        std::unique_ptr<sql::ResultSet> task(used->executeQuery());
        if (task->next()) return Result<void>::Failure({ErrorCode::CategoryNotEmpty, "该分类仍有任务，请先迁移任务"});
        std::unique_ptr<sql::PreparedStatement> remove(connection_->prepareStatement("DELETE FROM category WHERE id=?"));
        remove->setInt64(1, id);
        return remove->executeUpdate() == 1 ? Result<void>::Success() : Result<void>::Failure({ErrorCode::Conflict, "分类已发生变化"});
    } catch (const sql::SQLException& error) { return Result<void>::Failure(TranslateMySqlError(error)); }
}

Result<std::vector<CategoryCount>> MySqlCategoryRepository::ListWithCounts() {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement("SELECT c.id,c.name,c.color,c.created_at,COUNT(t.id) AS total,COALESCE(SUM(CASE WHEN t.completed=0 THEN 1 ELSE 0 END),0) AS pending,COALESCE(SUM(CASE WHEN t.completed=1 THEN 1 ELSE 0 END),0) AS completed FROM category c LEFT JOIN task t ON t.category_id=c.id GROUP BY c.id,c.name,c.color,c.created_at ORDER BY c.name,c.id"));
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        std::vector<CategoryCount> result;
        while (rows->next()) {
            auto category = ReadCategory(*rows);
            if (!category) continue;
            result.push_back({*category, rows->getInt64("total"), rows->getInt64("pending"), rows->getInt64("completed")});
        }
        return Result<std::vector<CategoryCount>>::Success(std::move(result));
    } catch (const sql::SQLException& error) { return Result<std::vector<CategoryCount>>::Failure(TranslateMySqlError(error)); }
}

Result<Category> MySqlCategoryRepository::GetById(std::int64_t id) {
    try {
        std::unique_ptr<sql::PreparedStatement> query(connection_->prepareStatement("SELECT id,name,color,created_at FROM category WHERE id=?"));
        query->setInt64(1, id);
        std::unique_ptr<sql::ResultSet> rows(query->executeQuery());
        if (!rows->next()) return Result<Category>::Failure({ErrorCode::NotFound, "分类不存在"});
        auto category = ReadCategory(*rows);
        if (!category) return Result<Category>::Failure({ErrorCode::QueryFailed, "分类记录已损坏"});
        return Result<Category>::Success(*category);
    } catch (const sql::SQLException& error) { return Result<Category>::Failure(TranslateMySqlError(error)); }
}

}  // namespace todo
