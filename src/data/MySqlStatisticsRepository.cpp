#include "data/MySqlStatisticsRepository.h"

#include <memory>

#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <string>

#include "data/MySqlErrors.h"
#include "domain/Statistics.h"

namespace todo {

MySqlStatisticsRepository::MySqlStatisticsRepository(sql::Connection* connection) : connection_(connection) {}

Result<StatsSnapshot> MySqlStatisticsRepository::GetSnapshot(const TimeRange& today, const TimeRange& sevenDays, const LocalDateTime& now) {
    try {
        StatsSnapshot snapshot;
        std::unique_ptr<sql::PreparedStatement> overview(connection_->prepareStatement("SELECT COUNT(*) AS total,COALESCE(SUM(CASE WHEN completed=0 THEN 1 ELSE 0 END),0) AS pending,COALESCE(SUM(CASE WHEN completed=1 THEN 1 ELSE 0 END),0) AS completed,COALESCE(SUM(CASE WHEN completed=0 AND due_time IS NOT NULL AND due_time<? THEN 1 ELSE 0 END),0) AS overdue,COALESCE(SUM(CASE WHEN completed=1 AND completed_at>=? AND completed_at<? THEN 1 ELSE 0 END),0) AS completed_today,COALESCE(SUM(CASE WHEN completed=0 AND due_time>=? AND due_time<? THEN 1 ELSE 0 END),0) AS today_pending FROM task"));
        overview->setString(1, now.ToString());
        overview->setString(2, today.start.ToString());
        overview->setString(3, today.end.ToString());
        overview->setString(4, today.start.ToString());
        overview->setString(5, today.end.ToString());
        std::unique_ptr<sql::ResultSet> totals(overview->executeQuery());
        if (!totals->next()) return Result<StatsSnapshot>::Failure(InternalDataError());
        const auto total = totals->getInt64("total");
        snapshot.overview.pending = totals->getInt64("pending");
        snapshot.overview.completed = totals->getInt64("completed");
        snapshot.overview.overdue = totals->getInt64("overdue");
        snapshot.overview.completedToday = totals->getInt64("completed_today");
        snapshot.overview.todayPending = totals->getInt64("today_pending");
        snapshot.completionRate = Statistics::CompletionRate(total, snapshot.overview.completed);

        std::unique_ptr<sql::PreparedStatement> categories(connection_->prepareStatement("SELECT c.id,c.name,c.color,COUNT(t.id) AS total,COALESCE(SUM(CASE WHEN t.completed=1 THEN 1 ELSE 0 END),0) AS completed FROM category c LEFT JOIN task t ON t.category_id=c.id GROUP BY c.id,c.name,c.color UNION ALL SELECT NULL,'未分类','#8E8E93',COUNT(*),COALESCE(SUM(CASE WHEN completed=1 THEN 1 ELSE 0 END),0) FROM task WHERE category_id IS NULL ORDER BY name"));
        std::unique_ptr<sql::ResultSet> categoryRows(categories->executeQuery());
        while (categoryRows->next()) {
            CategoryStat stat;
            if (!categoryRows->isNull(1)) stat.categoryId = categoryRows->getInt64(1);
            stat.name = categoryRows->getString(2).c_str();
            stat.color = categoryRows->getString(3).c_str();
            stat.total = categoryRows->getInt64(4);
            stat.completed = categoryRows->getInt64(5);
            snapshot.categories.push_back(std::move(stat));
        }

        std::unique_ptr<sql::PreparedStatement> trend(connection_->prepareStatement("SELECT DATE(event_at) AS day,COUNT(*) AS completed_events FROM history WHERE operation_type='COMPLETE' AND event_at>=? AND event_at<? GROUP BY DATE(event_at) ORDER BY day"));
        trend->setString(1, sevenDays.start.ToString());
        trend->setString(2, sevenDays.end.ToString());
        std::unique_ptr<sql::ResultSet> trendRows(trend->executeQuery());
        while (trendRows->next()) {
            const std::string text = trendRows->getString(1).c_str();
            auto day = LocalDateTime::Parse(text + " 00:00:00");
            if (day) snapshot.sevenDayTrend.push_back({*day, trendRows->getInt64(2)});
        }
        return Result<StatsSnapshot>::Success(std::move(snapshot));
    } catch (const sql::SQLException& error) { return Result<StatsSnapshot>::Failure(TranslateMySqlError(error)); }
}

}  // namespace todo
