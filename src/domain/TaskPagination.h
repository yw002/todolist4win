#pragma once

#include <optional>
#include <string>

#include "common/Result.h"
#include "domain/Models.h"

namespace todo::TaskPagination {

// Pure domain helpers for keyset pagination of TaskQuery. No Win32, D2D, SQL, MySQL or clock
// dependency lives here, so the tests can exercise the whole session contract offline.

// Signature of the query's filter set. It must cover every predicate bound by the repository's
// AppendFilter, otherwise a cached page from a different filter set gets reused.
// Today uses day granularity (its predicate is a whole-day range); Overdue uses the full
// microsecond-precision now because that is exactly what its predicate binds.
[[nodiscard]] std::string TaskQuerySignature(const TaskQuery& query);

// First page (!more) uses freshNow. A continuation page must reuse the session time frozen in
// the cursor; if the session time is missing, fail instead of silently binding a new clock value
// to an old cursor (which would shift the Overdue boundary and duplicate/skip rows).
[[nodiscard]] Result<LocalDateTime> ResolveTaskQueryNow(bool more, const std::optional<TaskCursor>& cursor, const LocalDateTime& freshNow);

// No cursor means a first page and is always compatible. With a cursor, both the filter signature
// and the frozen session time must match the query, otherwise the continuation is rejected.
[[nodiscard]] bool IsTaskCursorCompatible(const TaskQuery& query);

// Builds the only legitimate nextCursor: it copies all keyset fields and freezes query.now into
// sessionNow so the following page resolves to the same signature.
[[nodiscard]] TaskCursor MakeNextTaskCursor(const Task& last, const TaskQuery& query, const std::string& signature);

}  // namespace todo::TaskPagination
