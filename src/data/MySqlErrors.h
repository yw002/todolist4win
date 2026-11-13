#pragma once

#include <jdbc/cppconn/exception.h>

#include "common/Error.h"

namespace todo {
[[nodiscard]] AppError TranslateMySqlError(const sql::SQLException& error);
[[nodiscard]] AppError InternalDataError();
}
