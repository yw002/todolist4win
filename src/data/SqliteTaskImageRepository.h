#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/RepositoryInterfaces.h"

struct sqlite3;

namespace todo {

// PNG BLOBs for the local SQLite database. All three operations run inside the caller's UoW, so a
// failed save rolls back the blobs together with note/body_doc. Deleting a task is handled purely
// by fk_task_image_task ON DELETE CASCADE - this class never issues a task-delete GC.
class SqliteTaskImageRepository final : public ITaskImageRepository {
public:
    explicit SqliteTaskImageRepository(sqlite3* db);
    Result<void> Put(std::int64_t taskId, const BodyImage& image) override;
    Result<BodyImage> Get(std::int64_t taskId, const std::string& key) override;
    Result<void> DeleteUnreferenced(std::int64_t taskId, const std::vector<std::string>& keys) override;

private:
    sqlite3* db_{nullptr};
};

}  // namespace todo
