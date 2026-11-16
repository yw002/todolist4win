#pragma once

#include "common/Error.h"

namespace todo {

// Maps a SQLite result/extended result code to an AppError. The message is a fixed, user-facing
// summary; the raw SQL, bound parameters and file contents are never copied into it. The full
// numeric code is preserved in AppError.nativeCode for diagnostics.
[[nodiscard]] AppError TranslateSqliteResult(int resultCode);
// Refines a failed task_image write: SQLITE_TOOBIG means the BLOB exceeded SQLite's limit, and a
// CHECK violation means the row's size/dimension/key constraints were hit. Everything else is left
// as the generic mapping so connection and I/O failures keep their original meaning.
[[nodiscard]] AppError TranslateImagePutError(const AppError& failure);

// Generic "something went wrong while decoding a row" error for the SQLite layer.
[[nodiscard]] AppError InternalSqliteError();

}  // namespace todo
