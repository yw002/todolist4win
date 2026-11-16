#pragma once

#include <cstdint>
#include <vector>

#include "core/RepositoryInterfaces.h"

struct sqlite3;

namespace todo {

// SQLite implementation of ICategoryRepository: unique name
// collisions surface as Conflict, a category with tasks cannot be deleted, and the counts list is
// ordered by name then id.
class SqliteCategoryRepository final : public ICategoryRepository {
public:
    explicit SqliteCategoryRepository(sqlite3* db);
    Result<Category> Create(const Category& category) override;
    Result<Category> Update(const Category& category) override;
    Result<void> DeleteIfEmpty(std::int64_t id) override;
    Result<std::vector<CategoryCount>> ListWithCounts() override;
    Result<Category> GetById(std::int64_t id) override;

private:
    sqlite3* db_{nullptr};
};

}  // namespace todo
