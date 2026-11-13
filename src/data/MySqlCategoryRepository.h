#pragma once

#include <jdbc/cppconn/connection.h>
#include <vector>

#include "core/RepositoryInterfaces.h"

namespace todo {

class MySqlCategoryRepository final : public ICategoryRepository {
public:
    explicit MySqlCategoryRepository(sql::Connection* connection);
    Result<Category> Create(const Category& category) override;
    Result<Category> Update(const Category& category) override;
    Result<void> DeleteIfEmpty(std::int64_t id) override;
    Result<std::vector<CategoryCount>> ListWithCounts() override;
    Result<Category> GetById(std::int64_t id) override;
private:
    sql::Connection* connection_;
};

}  // namespace todo
