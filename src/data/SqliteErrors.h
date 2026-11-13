#pragma once

#include "common/Error.h"

namespace todo {

// Maps a SQLite result/extended result code to an AppError. The message is a fixed, user-facing
// summary; the raw SQL, bound parameters and file contents are never copied into it. The full
// numeric code is preserved in AppError.nativeCode for diagnostics.
[[nodiscard]] AppError TranslateSqliteResult(int resultCode);

// Generic "something went wrong while decoding a row" error, mirroring InternalDataError() on the
// MySQL side but without dragging in the JDBC headers.
[[nodiscard]] AppError InternalSqliteError();

}  // namespace todo
