#pragma once

#include "common/Result.h"
#include "data/MySqlConnection.h"

namespace todo {

class MySqlSchema {
public:
    static Result<void> EnsureDatabaseAndSchema(const DatabaseConfig& config);

private:
    static Result<void> EnsureTables(sql::Connection& connection);
};

}  // namespace todo
