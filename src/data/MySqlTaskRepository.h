#pragma once

#include <jdbc/cppconn/connection.h>

#include "core/RepositoryInterfaces.h"

namespace todo {

class MySqlTaskRepository final : public ITaskRepository {
public:
    explicit MySqlTaskRepository(sql::Connection* connection);
    Result<Task> Create(const Task& task) override;
    Result<Task> Update(const Task& task, std::uint64_t expectedRevision) override;
    Result<void> DeleteTask(std::int64_t id) override;
    Result<Task> GetByIdForUpdate(std::int64_t id) override;
    Result<TaskPage> QueryPage(const TaskQuery& request) override;
    Result<void> Reorder(const ReorderRequest& request) override;
private:
    Result<double> WeightOf(std::int64_t id);
    Result<void> NormalizeWeights();
    sql::Connection* connection_;
};

}  // namespace todo
