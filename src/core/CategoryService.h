#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Clock.h"
#include "common/Result.h"
#include "core/RepositoryInterfaces.h"

namespace todo {

class CategoryService {
public:
    CategoryService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock);
    Result<Category> Create(const std::string& name, const std::string& color);
    Result<Category> Update(std::int64_t id, const std::string& name, const std::string& color);
    Result<void> Delete(std::int64_t id);
    Result<std::vector<CategoryCount>> List();

private:
    IUnitOfWorkFactory& factory_;
    std::shared_ptr<IClock> clock_;
};

}  // namespace todo
