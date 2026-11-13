#pragma once

#include <memory>

#include "common/Clock.h"
#include "common/Result.h"
#include "core/RepositoryInterfaces.h"

namespace todo {

class StatisticsService {
public:
    StatisticsService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock);
    Result<StatsSnapshot> Refresh();

private:
    IUnitOfWorkFactory& factory_;
    std::shared_ptr<IClock> clock_;
};

}  // namespace todo
