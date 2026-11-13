#pragma once

#include <memory>

#include <jdbc/cppconn/connection.h>
#include <jdbc/mysql_driver.h>
#include <optional>
#include <string>

#include "common/Result.h"
#include "config/AppConfig.h"

namespace todo {

class MySqlConnection {
public:
    explicit MySqlConnection(DatabaseConfig config);
    Result<void> ConnectServer();
    Result<void> ConnectDatabase();
    [[nodiscard]] bool IsValid();
    sql::Connection& Get() noexcept;
    const DatabaseConfig& Config() const noexcept;

private:
    Result<void> Connect(const std::optional<std::string>& database);
    DatabaseConfig config_;
    sql::mysql::MySQL_Driver* driver_{nullptr};
    std::unique_ptr<sql::Connection> connection_;
};

}  // namespace todo
