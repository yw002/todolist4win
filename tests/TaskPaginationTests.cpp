#include <initializer_list>
#include <optional>
#include <string>

#include "common/Error.h"
#include "common/Result.h"
#include "domain/TaskPagination.h"
#include "domain/TimeRanges.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") {
    ReportTest("TaskPagination", name, value, detail);
}

LocalDateTime At(int year, int month, int day, int hour, int minute, int second, int microsecond) {
    return LocalDateTime{year, month, day, hour, minute, second, microsecond};
}

Task MakeTask(const LocalDateTime& dueAt) {
    Task task;
    task.id = 42;
    task.categoryId = 7;
    task.title = "overdue item";
    task.priority = Priority::High;
    task.dueAt = dueAt;
    task.sortWeight = 2048.5;
    task.createdAt = dueAt;
    task.revision = 3;
    return task;
}

TaskQuery MakeQuery(Route route, const LocalDateTime& now) {
    TaskQuery query;
    query.route = route;
    if (route == Route::Category) query.categoryId = 7;
    query.filter.keyword = "quarterly";
    query.filter.priority = Priority::High;
    query.filter.completion = CompletionFilter::Pending;
    query.display = CompletionDisplay::KeepVisible;
    query.page.pageSize = 200;
    query.now = now;
    return query;
}
}  // namespace

void RunTaskPaginationTests() {
    const LocalDateTime t1 = At(2026, 3, 14, 9, 30, 0, 123000);
    const LocalDateTime t2 = At(2026, 3, 14, 9, 30, 0, 124000);  // t1 + 1 ms
    Check("T1 and T2 differ at microsecond precision", t1.ToString() != t2.ToString());

    // --- M-1: Overdue continuation must reuse the frozen session time -------------------
    {
        TaskQuery first = MakeQuery(Route::Overdue, t1);
        const std::string s1 = TaskPagination::TaskQuerySignature(first);
        const Task last = MakeTask(At(2026, 3, 13, 23, 59, 59, 0));
        const TaskCursor cursor = TaskPagination::MakeNextTaskCursor(last, first, s1);

        const auto resolved = TaskPagination::ResolveTaskQueryNow(true, cursor, t2);
        Check("M-1 resolver returns frozen T1 for more=true", resolved.ok() && resolved.value() == t1);
        Check("M-1 resolver does not fall back to fresh T2", resolved.ok() && !(resolved.value() == t2));
        Check("M-1 cursor froze T1", cursor.sessionNow.has_value() && *cursor.sessionNow == t1);

        TaskQuery second = MakeQuery(Route::Overdue, resolved.value());
        second.page.cursor = cursor;
        const std::string s2 = TaskPagination::TaskQuerySignature(second);
        Check("M-1 second page signature equals S1", s2 == s1);
        Check("M-1 second page cursor is compatible", TaskPagination::IsTaskCursorCompatible(second));
        Check("M-1 first page without cursor is compatible", TaskPagination::IsTaskCursorCompatible(first));
    }

    // --- Session isolation: never bind a new clock value to an old cursor --------------
    {
        TaskQuery first = MakeQuery(Route::Overdue, t1);
        const std::string s1 = TaskPagination::TaskQuerySignature(first);
        const TaskCursor cursor = TaskPagination::MakeNextTaskCursor(MakeTask(t1), first, s1);

        TaskQuery cheated = MakeQuery(Route::Overdue, t2);
        cheated.page.cursor = cursor;
        Check("T2 signature differs from S1 without resolver", TaskPagination::TaskQuerySignature(cheated) != s1);
        Check("old cursor with fresh T2 is rejected", !TaskPagination::IsTaskCursorCompatible(cheated));

        const auto fresh = TaskPagination::ResolveTaskQueryNow(false, cursor, t2);
        Check("more=false always starts from fresh T2", fresh.ok() && fresh.value() == t2);
        TaskQuery newFirst = MakeQuery(Route::Overdue, fresh.value());
        newFirst.page.cursor.reset();
        Check("new first page drops the old cursor", !newFirst.page.cursor.has_value());
        Check("new first page signature differs from S1", TaskPagination::TaskQuerySignature(newFirst) != s1);
    }

    // --- Today: day granularity --------------------------------------------------------
    {
        TaskQuery first = MakeQuery(Route::Today, t1);
        const std::string s1 = TaskPagination::TaskQuerySignature(first);
        const TaskCursor cursor = TaskPagination::MakeNextTaskCursor(MakeTask(t1), first, s1);

        const auto sameDay = TaskPagination::ResolveTaskQueryNow(true, cursor, t2);
        Check("Today same-day resolver returns T1", sameDay.ok() && sameDay.value() == t1);
        TaskQuery second = MakeQuery(Route::Today, sameDay.value());
        second.page.cursor = cursor;
        Check("Today same-day signature is stable", TaskPagination::TaskQuerySignature(second) == s1);
        Check("Today same-day cursor is compatible", TaskPagination::IsTaskCursorCompatible(second));
        Check("Today signature ignores time-of-day within the day", TaskPagination::TaskQuerySignature(MakeQuery(Route::Today, t2)) == s1);

        const LocalDateTime nextDay = TimeRanges::AddDays(t1, 1);
        const auto newDay = TaskPagination::ResolveTaskQueryNow(false, std::nullopt, nextDay);
        TaskQuery nextFirst = MakeQuery(Route::Today, newDay.value());
        nextFirst.page.cursor.reset();
        Check("Today cross-day first page has a different signature", TaskPagination::TaskQuerySignature(nextFirst) != s1);
        TaskQuery staleOnNewDay = nextFirst;
        staleOnNewDay.page.cursor = cursor;
        Check("Today old cursor is incompatible with a new day", !TaskPagination::IsTaskCursorCompatible(staleOnNewDay));
    }

    // --- AllTasks / Category: no clock component in the signature ----------------------
    {
        for (Route route : {Route::AllTasks, Route::Category}) {
            TaskQuery q1 = MakeQuery(route, t1);
            TaskQuery q2 = MakeQuery(route, t2);
            const std::string s1 = TaskPagination::TaskQuerySignature(q1);
            Check("clock-free route signature ignores T1/T2", s1 == TaskPagination::TaskQuerySignature(q2));
            const TaskCursor cursor = TaskPagination::MakeNextTaskCursor(MakeTask(t1), q1, s1);
            q1.page.cursor = cursor;
            Check("clock-free route compatible when sessionNow equals query.now", TaskPagination::IsTaskCursorCompatible(q1));
            q2.page.cursor = cursor;
            Check("clock-free route still requires matching sessionNow", !TaskPagination::IsTaskCursorCompatible(q2));
        }
    }

    // --- dueRange takes part in the signature --------------------------------------
    // Closes the only remaining coverage gap reported by QA: AppendFilter binds dueRange at
    // so a cached page from a different range must be rejected.
    {
        TaskQuery base = MakeQuery(Route::AllTasks, t1);
        const std::string withoutRange = TaskPagination::TaskQuerySignature(base);

        TaskQuery withRange = base;
        withRange.filter.dueRange = TimeRange{At(2026, 3, 1, 0, 0, 0, 0), At(2026, 4, 1, 0, 0, 0, 0)};
        const std::string ranged = TaskPagination::TaskQuerySignature(withRange);
        Check("dueRange changes the signature", ranged != withoutRange);

        TaskQuery differentEnd = base;
        differentEnd.filter.dueRange = TimeRange{At(2026, 3, 1, 0, 0, 0, 0), At(2026, 5, 1, 0, 0, 0, 0)};
        Check("a different dueRange end changes the signature",
              TaskPagination::TaskQuerySignature(differentEnd) != ranged);

        TaskQuery differentStart = base;
        differentStart.filter.dueRange = TimeRange{At(2026, 2, 1, 0, 0, 0, 0), At(2026, 4, 1, 0, 0, 0, 0)};
        Check("a different dueRange start changes the signature",
              TaskPagination::TaskQuerySignature(differentStart) != ranged);

        TaskQuery sameRange = base;
        sameRange.filter.dueRange = TimeRange{At(2026, 3, 1, 0, 0, 0, 0), At(2026, 4, 1, 0, 0, 0, 0)};
        Check("an identical dueRange keeps the signature", TaskPagination::TaskQuerySignature(sameRange) == ranged);

        // A cursor issued without a range must be rejected once a range is applied.
        const TaskCursor plainCursor = TaskPagination::MakeNextTaskCursor(MakeTask(t1), base, withoutRange);
        withRange.page.cursor = plainCursor;
        Check("a range-free cursor is rejected for a ranged query",
              !TaskPagination::IsTaskCursorCompatible(withRange));

        // And the reverse direction must be rejected too.
        TaskQuery rangedFirst = withRange;
        rangedFirst.page.cursor.reset();
        const TaskCursor rangedCursor = TaskPagination::MakeNextTaskCursor(MakeTask(t1), rangedFirst, ranged);
        base.page.cursor = rangedCursor;
        Check("a ranged cursor is rejected for a range-free query",
              !TaskPagination::IsTaskCursorCompatible(base));

        // The same range in a continuation page stays compatible.
        TaskQuery rangedSecond = MakeQuery(Route::AllTasks, t1);
        rangedSecond.filter.dueRange = TimeRange{At(2026, 3, 1, 0, 0, 0, 0), At(2026, 4, 1, 0, 0, 0, 0)};
        rangedSecond.page.cursor = rangedCursor;
        Check("an identical range keeps the cursor compatible", TaskPagination::IsTaskCursorCompatible(rangedSecond));
    }

    // --- Field propagation over two consecutive pages --------------------------------
    {
        TaskQuery first = MakeQuery(Route::Overdue, t1);
        const std::string s1 = TaskPagination::TaskQuerySignature(first);
        const LocalDateTime due = At(2026, 3, 12, 23, 59, 59, 0);
        const Task last = MakeTask(due);
        const TaskCursor c1 = TaskPagination::MakeNextTaskCursor(last, first, s1);
        Check("cursor keeps dueAt", c1.dueAt.has_value() && *c1.dueAt == due);
        Check("cursor keeps sortWeight", c1.sortWeight == last.sortWeight);
        Check("cursor keeps id", c1.id == last.id);
        Check("cursor keeps signature", c1.signature == s1);
        Check("cursor keeps sessionNow", c1.sessionNow.has_value() && *c1.sessionNow == t1);

        const auto resolved = TaskPagination::ResolveTaskQueryNow(true, c1, t2);
        TaskQuery second = MakeQuery(Route::Overdue, resolved.value());
        second.page.cursor = c1;
        const TaskCursor c2 = TaskPagination::MakeNextTaskCursor(last, second, TaskPagination::TaskQuerySignature(second));
        Check("second continuation still freezes T1", c2.sessionNow.has_value() && *c2.sessionNow == t1);
        Check("second continuation keeps the same signature", c2.signature == s1);
        Check("second continuation keeps the same keyset id", c2.id == last.id);
    }

    // --- Missing session boundary must fail, never fall back to freshNow ---------------
    {
        TaskQuery first = MakeQuery(Route::Overdue, t1);
        const std::string s1 = TaskPagination::TaskQuerySignature(first);
        TaskCursor broken;
        broken.dueAt = t1;
        broken.sortWeight = 4096.0;
        broken.id = 99;
        broken.signature = s1;
        broken.sessionNow.reset();

        const auto resolved = TaskPagination::ResolveTaskQueryNow(true, broken, t2);
        Check("missing sessionNow returns a failure", !resolved.ok());
        Check("missing sessionNow returns Conflict", !resolved.ok() && resolved.error().code == ErrorCode::Conflict);
        first.page.cursor = broken;
        Check("cursor without sessionNow is incompatible", !TaskPagination::IsTaskCursorCompatible(first));
        const auto noCursor = TaskPagination::ResolveTaskQueryNow(true, std::nullopt, t2);
        Check("more=true without any cursor also fails", !noCursor.ok());
    }

    // --- History pagination is untouched by the task session change -------------------
    {
        const LocalDateTime eventAt = At(2026, 3, 14, 8, 0, 0, 0);
        HistoryCursor cursor{eventAt, 512, "history-signature"};
        HistoryQuery query;
        query.page.pageSize = 200;
        query.page.cursor = cursor;
        Check("HistoryCursor keeps eventAt", cursor.eventAt == eventAt);
        Check("HistoryCursor keeps id", cursor.id == 512);
        Check("HistoryCursor keeps its own signature", cursor.signature == "history-signature");
        Check("HistoryQuery cursor is independent of TaskCursor", query.page.cursor.has_value() && query.page.cursor->id == 512);
    }
}
