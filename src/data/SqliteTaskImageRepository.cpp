#include "data/SqliteTaskImageRepository.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "common/Sha256.h"
#include "domain/ImageValidation.h"
#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"

namespace todo {
namespace {

[[nodiscard]] Result<std::int64_t> UsedBytes(sqlite3* db, std::int64_t taskId) {
    SqliteStatement query(db, "SELECT COALESCE(SUM(byte_size),0) FROM task_image WHERE task_id=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<std::int64_t>::Failure(prepared.error());
    query.BindInt64(1, taskId);
    auto row = query.Step();
    if (!row.ok()) return Result<std::int64_t>::Failure(row.error());
    return Result<std::int64_t>::Success(row.value() ? query.ColumnInt64(0) : 0);
}

[[nodiscard]] Result<std::int64_t> UsedCount(sqlite3* db, std::int64_t taskId) {
    SqliteStatement query(db, "SELECT COUNT(*) FROM task_image WHERE task_id=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<std::int64_t>::Failure(prepared.error());
    query.BindInt64(1, taskId);
    auto row = query.Step();
    if (!row.ok()) return Result<std::int64_t>::Failure(row.error());
    return Result<std::int64_t>::Success(row.value() ? query.ColumnInt64(0) : 0);
}

}  // namespace

SqliteTaskImageRepository::SqliteTaskImageRepository(sqlite3* db) : db_(db) {}

Result<void> SqliteTaskImageRepository::Put(std::int64_t taskId, const BodyImage& image) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    const auto valid = ValidateBodyImage(image);
    if (!valid.ok()) return valid;
    // INSERT OR IGNORE is the whole idempotency story: an existing (task_id, image_key) keeps its
    // original bytes instead of being rewritten with a different blob under the same content key.
    SqliteStatement exists(db_, "SELECT 1 FROM task_image WHERE task_id=? AND image_key=?;");
    auto prepared = exists.Prepare();
    if (!prepared.ok()) return prepared;
    exists.BindInt64(1, taskId);
    exists.BindText(2, image.key);
    auto row = exists.Step();
    if (!row.ok()) return Result<void>::Failure(row.error());
    if (row.value()) return Result<void>::Success();

    const auto count = UsedCount(db_, taskId);
    if (!count.ok()) return Result<void>::Failure(count.error());
    if (count.value() >= static_cast<std::int64_t>(kMaxImagesPerTask)) {
        return Result<void>::Failure({ErrorCode::BodyTooLarge, "单个任务最多 20 张图片"});
    }
    const auto used = UsedBytes(db_, taskId);
    if (!used.ok()) return Result<void>::Failure(used.error());
    if (static_cast<std::uint64_t>(used.value()) + image.bytes.size() > kMaxTaskImageBytes) {
        return Result<void>::Failure({ErrorCode::BodyTooLarge, "单个任务图片总量超过 16 MB"});
    }

    SqliteStatement insert(db_,
                           "INSERT OR IGNORE INTO task_image(task_id,image_key,mime,width,height,byte_size,data,created_at) "
                           "VALUES (?,?,?,?,?,?,?,datetime('now','localtime'));");
    auto ready = insert.Prepare();
    if (!ready.ok()) return ready;
    insert.BindInt64(1, taskId);
    insert.BindText(2, image.key);
    insert.BindText(3, image.mime);
    insert.BindInt64(4, static_cast<std::int64_t>(image.width));
    insert.BindInt64(5, static_cast<std::int64_t>(image.height));
    insert.BindInt64(6, static_cast<std::int64_t>(image.bytes.size()));
    insert.BindBlob(7, image.bytes);
    auto ran = insert.Run();
    if (!ran.ok()) return Result<void>::Failure(TranslateImagePutError(ran.error()));
    return ran;
}

Result<BodyImage> SqliteTaskImageRepository::Get(std::int64_t taskId, const std::string& key) {
    if (db_ == nullptr) return Result<BodyImage>::Failure(InternalSqliteError());
    if (!Sha256::IsCanonicalKey(key)) {
        return Result<BodyImage>::Failure({ErrorCode::ImageInvalid, "图片引用 key 格式无效"});
    }
    SqliteStatement query(db_,
                          "SELECT image_key,mime,width,height,byte_size,data FROM task_image WHERE task_id=? AND image_key=?;");
    auto prepared = query.Prepare();
    if (!prepared.ok()) return Result<BodyImage>::Failure(prepared.error());
    query.BindInt64(1, taskId);
    query.BindText(2, key);
    auto row = query.Step();
    if (!row.ok()) return Result<BodyImage>::Failure(row.error());
    if (!row.value()) {
        return Result<BodyImage>::Failure({ErrorCode::ImageUnavailable, "图片不可用"});
    }
    const std::int64_t declared = query.ColumnInt64(4);
    std::vector<std::byte> bytes = query.ColumnBlob(5);
    if (declared != static_cast<std::int64_t>(bytes.size())) {
        return Result<BodyImage>::Failure({ErrorCode::ImageInvalid, "图片数据长度不一致"});
    }
    if (Sha256::Hex(bytes) != key) {
        return Result<BodyImage>::Failure({ErrorCode::ImageInvalid, "图片 key 与 PNG 内容不匹配"});
    }
    BodyImage image;
    image.key = query.ColumnText(0);
    image.mime = query.ColumnText(1);
    image.width = static_cast<std::uint32_t>(query.ColumnInt64(2));
    image.height = static_cast<std::uint32_t>(query.ColumnInt64(3));
    image.bytes = std::move(bytes);
    return Result<BodyImage>::Success(std::move(image));
}

Result<void> SqliteTaskImageRepository::DeleteUnreferenced(std::int64_t taskId, const std::vector<std::string>& keys) {
    if (db_ == nullptr) return Result<void>::Failure(InternalSqliteError());
    if (keys.size() > kMaxImagesPerTask) {
        return Result<void>::Failure({ErrorCode::BodyTooLarge, "单个任务最多 20 张图片"});
    }
    std::ostringstream sql;
    sql << "DELETE FROM task_image WHERE task_id=?";
    if (!keys.empty()) {
        sql << " AND image_key NOT IN (";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i != 0) sql << ',';
            sql << '?';
        }
        sql << ')';
    }
    sql << ';';
    SqliteStatement remove(db_, sql.str());
    auto prepared = remove.Prepare();
    if (!prepared.ok()) return prepared;
    remove.BindInt64(1, taskId);
    for (std::size_t i = 0; i < keys.size(); ++i) remove.BindText(static_cast<int>(i) + 2, keys[i]);
    return remove.Run();
}

}  // namespace todo
