#include "core/StatisticsService.h"

#include "domain/Statistics.h"
#include "domain/TimeRanges.h"

#include <memory>

namespace todo {

StatisticsService::StatisticsService(IUnitOfWorkFactory& factory, std::shared_ptr<IClock> clock) : factory_(factory), clock_(std::move(clock)) {}

Result<StatsSnapshot> StatisticsService::Refresh() {
    auto created = factory_.Create();
    if (!created.ok()) return Result<StatsSnapshot>::Failure(created.error());
    const auto now = clock_->NowLocal();
    const auto today = TimeRanges::Today(now);
    const auto sevenDays = TimeRanges::SevenDays(now);
    auto& uow = *created.value();
    auto begun = uow.Begin();
    if (!begun.ok()) return Result<StatsSnapshot>::Failure(begun.error());
    TransactionGuard guard(uow);
    auto result = uow.Statistics().GetSnapshot(today, sevenDays, now);
    if (!result.ok()) return result;
    auto committed = uow.Commit();
    if (!committed.ok()) return Result<StatsSnapshot>::Failure(committed.error());
    guard.Committed();
    result.value().sevenDayTrend = Statistics::FillMissingDays(sevenDays, result.value().sevenDayTrend);
    return result;
}

}  // namespace todo
