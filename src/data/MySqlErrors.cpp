#include "data/MySqlErrors.h"

#include <string>

namespace todo {

AppError TranslateMySqlError(const sql::SQLException& error) {
    const int code = error.getErrorCode();
    ErrorCode translated = ErrorCode::QueryFailed;
    std::string message = "数据库操作失败，请稍后重试";
    if (code == 1045 || code == 1044) {
        translated = code == 1045 ? ErrorCode::AuthenticationFailed : ErrorCode::PermissionDenied;
        message = code == 1045 ? "数据库账号或密码不正确" : "当前账号没有所需的数据库权限";
    } else if (code == 1142 || code == 1143 || code == 1227 || code == 1370) {
        translated = ErrorCode::PermissionDenied;
        message = "数据库账号权限不足";
    } else if (code == 2002 || code == 2003 || code == 2006 || code == 2013 || code == 2055) {
        translated = ErrorCode::ConnectionFailed;
        message = "无法连接数据库，请检查服务和连接设置";
    } else if (code == 1205 || code == 1213) {
        translated = ErrorCode::Conflict;
        message = "数据库暂时繁忙，请重试";
    } else if (code == 1062) {
        translated = ErrorCode::Conflict;
        message = "名称或记录已存在";
    } else if (code == 1451) {
        translated = ErrorCode::CategoryNotEmpty;
        message = "该分类仍有任务，请先迁移任务";
    } else if (code == 1452) {
        translated = ErrorCode::InvalidInput;
        message = "所选分类已不存在";
    }
    return {translated, message, code, error.getSQLState()};
}

AppError InternalDataError() { return {ErrorCode::Internal, "数据库数据处理失败"}; }

}  // namespace todo
