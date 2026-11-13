#pragma once

#include <cstdint>
#include <vector>

#include "core/RepositoryInterfaces.h"

struct sqlite3;

namespace todo {

// SQLite implementation of IHistoryRepository. Pagination uses the shared HistoryPagination
// signature so MySQL and SQLite cannot drift; append/delete/latest mirror the MySQL semantics.
class SqliteHistoryRepository final : public IHistoryRepository {
public:
    explicit SqliteHistoryRepository(sqlite3* db);
    Result<HistoryEvent> Append(const HistoryEvent& event) override;
    Result<HistoryPage> QueryPage(const HistoryQuery& request) override;
    Result<void> DeleteEvents(const std::vector<std::int64_t>& ids) override;
    Result<HistoryEvent> GetLatestForTask(std::int64_t taskId) override;

private:
    sqlite3* db_{nullptr};
};

}  // namespace todo
