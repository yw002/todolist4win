#include "core/CategoryService.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "domain/TaskRules.h"

namespace todo {

CategoryService::CategoryService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock) : factory_(factory), clock_(std::move(clock)) {}

Result<Category> CategoryService::Create(const std::string& name, const std::string& color) {
    auto nameResult = TaskRules::ValidateCategoryName(name);
    if (!nameResult.ok()) return Result<Category>::Failure(nameResult.error());
    auto colorResult = TaskRules::ValidateColor(color);
    if (!colorResult.ok()) return Result<Category>::Failure(colorResult.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Category>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Category>::Failure(begun.error());
    TransactionGuard guard(uow);
    Category category;
    category.name = TaskRules::Trim(name);
    category.color = color;
    category.createdAt = clock_->NowLocal();
    auto result = uow.Categories().Create(category);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Category>::Failure(committed.error());
    guard.Committed();
    return result;
}

Result<Category> CategoryService::Update(std::int64_t id, const std::string& name, const std::string& color) {
    auto idResult = TaskRules::ValidateId(id);
    if (!idResult.ok()) return Result<Category>::Failure(idResult.error());
    auto nameResult = TaskRules::ValidateCategoryName(name);
    if (!nameResult.ok()) return Result<Category>::Failure(nameResult.error());
    auto colorResult = TaskRules::ValidateColor(color);
    if (!colorResult.ok()) return Result<Category>::Failure(colorResult.error());
    auto created = factory_.Create();
    if (!created.ok()) return Result<Category>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<Category>::Failure(begun.error());
    TransactionGuard guard(uow);
    Category category;
    category.id = id;
    category.name = TaskRules::Trim(name);
    category.color = color;
    auto result = uow.Categories().Update(category);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<Category>::Failure(committed.error());
    guard.Committed();
    return result;
}

Result<void> CategoryService::Delete(std::int64_t id) {
    auto idResult = TaskRules::ValidateId(id);
    if (!idResult.ok()) return idResult;
    auto created = factory_.Create();
    if (!created.ok()) return Result<void>::Failure(created.error());
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return begun;
    TransactionGuard guard(uow);
    auto result = uow.Categories().DeleteIfEmpty(id);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return committed;
    guard.Committed();
    return Result<void>::Success();
}

Result<std::vector<CategoryCount>> CategoryService::List() {
    auto created = factory_.Create();
    if (!created.ok()) return Result<std::vector<CategoryCount>>::Failure(created.error());
    return created.value()->Categories().ListWithCounts();
}

}  // namespace todo
