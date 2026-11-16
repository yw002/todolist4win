#pragma once

#include <cstdint>

#include "core/RepositoryInterfaces.h"

struct sqlite3;

namespace todo {

// SQLite implementation of ITaskRepository. Semantics follow the repository contract exactly: the same
// optimistic-lock revision check, the same keyset pagination through TaskPagination, the same
// midpoint weight insertion with a NormalizeWeights fallback.
class SqliteTaskRepository final : public ITaskRepository {
public:
    explicit SqliteTaskRepository(sqlite3* db);
    Result<Task> Create(const Task& task) override;
    Result<Task> Update(const Task& task, std::uint64_t expectedRevision) override;
    Result<void> DeleteTask(std::int64_t id) override;
    Result<Task> GetByIdForUpdate(std::int64_t id) override;
    Result<TaskPage> QueryPage(const TaskQuery& request) override;
    Result<void> Reorder(const ReorderRequest& request) override;

private:
    Result<double> WeightOf(std::int64_t id);
    Result<void> NormalizeWeights();
    sqlite3* db_{nullptr};
};

}  // namespace todo
