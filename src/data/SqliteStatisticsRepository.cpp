#include "data/SqliteStatisticsRepository.h"

#include <string>
#include <utility>

#include <sqlite3.h>

#include "data/SqliteConnection.h"
#include "data/SqliteErrors.h"
#include "domain/Statistics.h"

namespace todo {

SqliteStatisticsRepository::SqliteStatisticsRepository(sqlite3* db) : db_(db) {}

Result<StatsSnapshot> SqliteStatisticsRepository::GetSnapshot(const TimeRange& today, const TimeRange& sevenDays, const LocalDateTime& now) {
    if (db_ == nullptr) return Result<StatsSnapshot>::Failure(InternalSqliteError());
    StatsSnapshot snapshot;

    SqliteStatement overview(db_,
                             "SELECT COUNT(*) AS total,"
                             "COALESCE(SUM(CASE WHEN completed=0 THEN 1 ELSE 0 END),0) AS pending,"
                             "COALESCE(SUM(CASE WHEN completed=1 THEN 1 ELSE 0 END),0) AS completed,"
                             "COALESCE(SUM(CASE WHEN completed=0 AND due_time IS NOT NULL AND due_time<? THEN 1 ELSE 0 END),0) AS overdue,"
                             "COALESCE(SUM(CASE WHEN completed=1 AND completed_at>=? AND completed_at<? THEN 1 ELSE 0 END),0) AS completed_today,"
                             "COALESCE(SUM(CASE WHEN completed=0 AND due_time>=? AND due_time<? THEN 1 ELSE 0 END),0) AS today_pending "
                             "FROM task;");
    auto preparedOverview = overview.Prepare();
    if (!preparedOverview.ok()) return Result<StatsSnapshot>::Failure(preparedOverview.error());
    overview.BindText(1, now.ToString());
    overview.BindText(2, today.start.ToString());
    overview.BindText(3, today.end.ToString());
    overview.BindText(4, today.start.ToString());
    overview.BindText(5, today.end.ToString());
    auto totals = overview.Step();
    if (!totals.ok()) return Result<StatsSnapshot>::Failure(totals.error());
    if (!totals.value()) return Result<StatsSnapshot>::Failure(InternalSqliteError());
    const std::int64_t total = overview.ColumnInt64(0);
    snapshot.overview.pending = overview.ColumnInt64(1);
    snapshot.overview.completed = overview.ColumnInt64(2);
    snapshot.overview.overdue = overview.ColumnInt64(3);
    snapshot.overview.completedToday = overview.ColumnInt64(4);
    snapshot.overview.todayPending = overview.ColumnInt64(5);
    snapshot.completionRate = Statistics::CompletionRate(total, snapshot.overview.completed);

    SqliteStatement categories(db_,
                               "SELECT c.id,c.name,c.color,COUNT(t.id) AS total,"
                               "COALESCE(SUM(CASE WHEN t.completed=1 THEN 1 ELSE 0 END),0) AS completed "
                               "FROM category c LEFT JOIN task t ON t.category_id=c.id "
                               "GROUP BY c.id,c.name,c.color "
                               "UNION ALL "
                               "SELECT NULL,'未分类','#8E8E93',COUNT(*),COALESCE(SUM(CASE WHEN completed=1 THEN 1 ELSE 0 END),0) "
                               "FROM task WHERE category_id IS NULL "
                               "ORDER BY name;");
    auto preparedCategories = categories.Prepare();
    if (!preparedCategories.ok()) return Result<StatsSnapshot>::Failure(preparedCategories.error());
    for (;;) {
        auto row = categories.Step();
        if (!row.ok()) return Result<StatsSnapshot>::Failure(row.error());
        if (!row.value()) break;
        CategoryStat stat;
        if (!categories.ColumnIsNull(0)) stat.categoryId = categories.ColumnInt64(0);
        stat.name = categories.ColumnText(1);
        stat.color = categories.ColumnText(2);
        stat.total = categories.ColumnInt64(3);
        stat.completed = categories.ColumnInt64(4);
        snapshot.categories.push_back(std::move(stat));
    }

    SqliteStatement trend(db_,
                          "SELECT substr(event_at,1,10) AS day,COUNT(*) AS completed_events "
                          "FROM history WHERE operation_type='COMPLETE' AND event_at>=? AND event_at<? "
                          "GROUP BY substr(event_at,1,10) ORDER BY day;");
    auto preparedTrend = trend.Prepare();
    if (!preparedTrend.ok()) return Result<StatsSnapshot>::Failure(preparedTrend.error());
    trend.BindText(1, sevenDays.start.ToString());
    trend.BindText(2, sevenDays.end.ToString());
    for (;;) {
        auto row = trend.Step();
        if (!row.ok()) return Result<StatsSnapshot>::Failure(row.error());
        if (!row.value()) break;
        auto day = LocalDateTime::Parse(trend.ColumnText(0) + " 00:00:00");
        if (day) snapshot.sevenDayTrend.push_back({*day, trend.ColumnInt64(1)});
    }
    return Result<StatsSnapshot>::Success(std::move(snapshot));
}

}  // namespace todo
