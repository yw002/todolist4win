#pragma once

#include <jdbc/cppconn/connection.h>
#include <vector>

#include "core/RepositoryInterfaces.h"

namespace todo {

class MySqlHistoryRepository final : public IHistoryRepository {
public:
    explicit MySqlHistoryRepository(sql::Connection* connection);
    Result<HistoryEvent> Append(const HistoryEvent& event) override;
    Result<HistoryPage> QueryPage(const HistoryQuery& request) override;
    Result<void> DeleteEvents(const std::vector<std::int64_t>& ids) override;
    Result<HistoryEvent> GetLatestForTask(std::int64_t taskId) override;
private:
    sql::Connection* connection_;
};

}  // namespace todo
