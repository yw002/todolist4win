#pragma once

#include <string>

#include "domain/Models.h"

namespace todo::HistoryPagination {

// Pure, database-agnostic helpers for history keyset pagination. The SQLite history repository
// calls these so the query signature and cursor contract stay in one place.
// No Win32, SQL or clock dependency lives here.

// Stable hash of every user-controlled history filter: keyword, category and time range.
[[nodiscard]] std::string QuerySignature(const HistoryQuery& query);

// A first page (no cursor) is always compatible. With a cursor the stored signature must match the
// current filter set, otherwise a cached page from another filter set would be reused.
[[nodiscard]] bool IsCursorCompatible(const HistoryQuery& query);

// Builds the only legitimate next cursor from the last returned event.
[[nodiscard]] HistoryCursor MakeNextHistoryCursor(const HistoryEvent& last, const HistoryQuery& query, const std::string& signature);

}  // namespace todo::HistoryPagination
