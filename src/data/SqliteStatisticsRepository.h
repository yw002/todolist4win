#pragma once

#include "core/RepositoryInterfaces.h"

struct sqlite3;

namespace todo {

// SQLite implementation of IStatisticsRepository. Completion rate is computed from the current
// task rows (not the history events), and the 7-day trend counts COMPLETE events.
class SqliteStatisticsRepository final : public IStatisticsRepository {
public:
    explicit SqliteStatisticsRepository(sqlite3* db);
    Result<StatsSnapshot> GetSnapshot(const TimeRange& today, const TimeRange& sevenDays, const LocalDateTime& now) override;

private:
    sqlite3* db_{nullptr};
};

}  // namespace todo
