#include "data/MySqlConnection.h"

#include <jdbc/cppconn/statement.h>
#include <optional>
#include <string>

#include "data/MySqlErrors.h"

namespace todo {

MySqlConnection::MySqlConnection(DatabaseConfig config) : config_(std::move(config)) {}

Result<void> MySqlConnection::Connect(const std::optional<std::string>& database) {
    try {
        driver_ = sql::mysql::get_mysql_driver_instance();
        sql::ConnectOptionsMap options;
        options["hostName"] = "tcp://" + config_.host + ":" + std::to_string(config_.port);
        options["userName"] = config_.username;
        options["password"] = config_.password;
        options["OPT_CONNECT_TIMEOUT"] = config_.connectTimeoutSeconds;
        options["OPT_READ_TIMEOUT"] = config_.connectTimeoutSeconds;
        options["OPT_WRITE_TIMEOUT"] = config_.connectTimeoutSeconds;
        options["OPT_CHARSET_NAME"] = "utf8mb4";
        if (database) options["schema"] = *database;
        connection_.reset(driver_->connect(options));
        connection_->setAutoCommit(true);
        return Result<void>::Success();
    } catch (const sql::SQLException& error) {
        connection_.reset();
        return Result<void>::Failure(TranslateMySqlError(error));
    } catch (...) {
        connection_.reset();
        return Result<void>::Failure(InternalDataError());
    }
}

Result<void> MySqlConnection::ConnectServer() { return Connect(std::nullopt); }
Result<void> MySqlConnection::ConnectDatabase() { return Connect(config_.database); }

bool MySqlConnection::IsValid() {
    try { return connection_ && !connection_->isClosed() && connection_->isValid(); }
    catch (...) { return false; }
}

sql::Connection& MySqlConnection::Get() noexcept { return *connection_; }
const DatabaseConfig& MySqlConnection::Config() const noexcept { return config_; }

}  // namespace todo
