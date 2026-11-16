#pragma once

#include <filesystem>

#include "common/Result.h"

namespace todo {

// Creates the local database (if needed) and its schema, then verifies it belongs to this
// application. The operation is idempotent and safe to call on every startup.
class SqliteSchema {
public:
    // application_id 0x54444C57 ("TDLW") marks a database owned by this app. It is written once on
    // creation; a file with a different id is rejected with SchemaUnsupported rather than modified.
    static constexpr int kApplicationId = 0x54444C57;
    // V1 = base task/history/category schema; V2 adds task.body_doc and task_image. A database with
    // a higher version is never touched (a newer binary owns that format).
    static constexpr int kSchemaVersion = 2;

    [[nodiscard]] static Result<void> EnsureDatabaseAndSchema(const std::filesystem::path& path);
};

}  // namespace todo
