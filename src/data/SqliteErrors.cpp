#include "data/SqliteErrors.h"

#include <sqlite3.h>

namespace todo {

AppError TranslateSqliteResult(int resultCode) {
    ErrorCode translated = ErrorCode::QueryFailed;
    std::string message = "本地数据库操作失败，请稍后重试";
    switch (resultCode) {
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            translated = ErrorCode::Conflict;
            message = "本地数据库繁忙，请重试";
            break;
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
            translated = ErrorCode::Conflict;
            message = "名称或记录已存在";
            break;
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            // The category repository pre-checks emptiness, so the common FK failure here is an
            // insert/update pointing at a category that no longer exists.
            translated = ErrorCode::InvalidInput;
            message = "所选分类已不存在";
            break;
        case SQLITE_CONSTRAINT_CHECK:
        case SQLITE_CONSTRAINT_NOTNULL:
            translated = ErrorCode::InvalidInput;
            message = "输入数据不符合本地数据库约束";
            break;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            translated = ErrorCode::QueryFailed;
            message = "本地数据库已损坏，请勿删除数据文件";
            break;
        case SQLITE_READONLY:
        case SQLITE_PERM:
            translated = ErrorCode::PermissionDenied;
            message = "无法写入本地数据目录";
            break;
        case SQLITE_CANTOPEN:
            translated = ErrorCode::ConnectionFailed;
            message = "本地数据库无法打开";
            break;
        case SQLITE_FULL:
            translated = ErrorCode::QueryFailed;
            message = "磁盘空间不足，无法写入本地数据库";
            break;
        case SQLITE_IOERR:
            translated = ErrorCode::QueryFailed;
            message = "本地数据库读写失败";
            break;
        case SQLITE_NOMEM:
            translated = ErrorCode::Internal;
            message = "内存不足，本地数据库操作失败";
            break;
        default:
            if ((resultCode & 0xFF) == SQLITE_CONSTRAINT) {
                translated = ErrorCode::Conflict;
                message = "本地数据约束冲突";
            }
            break;
    }
    return {translated, message, resultCode, "SQLITE"};
}

AppError InternalSqliteError() { return {ErrorCode::Internal, "本地数据库数据处理失败"}; }

}  // namespace todo
