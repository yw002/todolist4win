#pragma once

#include <jdbc/cppconn/connection.h>

#include "core/RepositoryInterfaces.h"

namespace todo {

class MySqlStatisticsRepository final : public IStatisticsRepository {
public:
    explicit MySqlStatisticsRepository(sql::Connection* connection);
    Result<StatsSnapshot> GetSnapshot(const TimeRange& today, const TimeRange& sevenDays, const LocalDateTime& now) override;
private:
    sql::Connection* connection_;
};

}  // namespace todo
