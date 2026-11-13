// Offline tests for the shared HistoryPagination signature/cursor helpers. No database is opened;
// these assert that the signature covers every user-controlled filter and that the cursor contract
// is stable, then the SQLite repository tests exercise the same helpers against a real database.

#include <cstdint>
#include <string>

#include "domain/HistoryPagination.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("HistoryPagination", name, value, detail); }

LocalDateTime At(int year, int month, int day, int hour, int minute, int second, int microsecond) {
    return LocalDateTime{year, month, day, hour, minute, second, microsecond};
}
}  // namespace

void RunHistoryPaginationTests() {
    HistoryQuery base;
    base.filter.keyword = "report";
    base.filter.categoryId = 7;
    const std::string signature = HistoryPagination::QuerySignature(base);

    HistoryCursor cursor{At(2026, 6, 1, 9, 0, 0, 0), 42, signature};
    base.page.cursor = cursor;
    Check("matching signature cursor is compatible", HistoryPagination::IsCursorCompatible(base));

    HistoryQuery changedKeyword = base;
    changedKeyword.filter.keyword = "other";
    Check("keyword changes the signature", HistoryPagination::QuerySignature(changedKeyword) != signature);
    Check("changed keyword cursor is incompatible", !HistoryPagination::IsCursorCompatible(changedKeyword));

    HistoryQuery changedCategory = base;
    changedCategory.filter.categoryId = 8;
    Check("category changes the signature", HistoryPagination::QuerySignature(changedCategory) != signature);

    HistoryQuery timed;
    timed.filter.keyword = "report";
    timed.filter.categoryId = 7;
    timed.filter.timeRange = TimeRange{At(2026, 6, 1, 0, 0, 0, 0), At(2026, 6, 2, 0, 0, 0, 0)};
    Check("time range changes the signature", HistoryPagination::QuerySignature(timed) != signature);

    HistoryQuery noCursor;
    noCursor.filter = base.filter;
    Check("a first page with no cursor is always compatible", HistoryPagination::IsCursorCompatible(noCursor));

    HistoryEvent event;
    event.id = 99;
    event.eventAt = At(2026, 6, 1, 10, 30, 0, 250000);
    const HistoryCursor next = HistoryPagination::MakeNextHistoryCursor(event, noCursor, signature);
    Check("next cursor carries the event time", next.eventAt == event.eventAt);
    Check("next cursor carries the id", next.id == event.id);
    Check("next cursor carries the signature", next.signature == signature);

    // A cursor built from the same filter set is compatible again (round trip).
    HistoryQuery continued = base;
    continued.page.cursor = next;
    Check("round-tripped cursor stays compatible", HistoryPagination::IsCursorCompatible(continued));
}
