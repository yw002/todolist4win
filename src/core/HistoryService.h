#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/Clock.h"
#include "common/Result.h"
#include "core/RepositoryInterfaces.h"

namespace todo {

class HistoryService {
public:
    HistoryService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock);
    Result<Task> Restore(std::int64_t taskId);
    BatchResult RestoreBatch(const std::vector<std::int64_t>& ids);
    Result<void> DeleteEvents(const std::vector<std::int64_t>& ids);
    Result<HistoryPage> Query(const HistoryQuery& query);

private:
    IUnitOfWorkFactory& factory_;
    std::shared_ptr<IClock> clock_;
};

}  // namespace todo
