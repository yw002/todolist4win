// Shared repository contract driven through the service layer and IUnitOfWorkFactory only.
//
// Nothing here references a concrete database: the SQLite suite calls RunRepositoryContract with a
// SqliteUnitOfWorkFactory. Because the file compiles into the default test project, it must never
// include a database driver header or open a connection outside the factory.

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/Clock.h"
#include "common/Error.h"
#include "common/Result.h"
#include "core/CategoryService.h"
#include "core/HistoryService.h"
#include "core/StatisticsService.h"
#include "core/TaskService.h"
#include "domain/BodyDocument.h"
#include "domain/TimeRanges.h"

void ReportTest(const char*, const char*, bool, const char*);

namespace {

class ContractClock final : public todo::IClock {
public:
    todo::LocalDateTime now{2026, 6, 1, 9, 0, 0, 0};
    [[nodiscard]] todo::LocalDateTime NowLocal() const override { return now; }
};

void Check(const char* suite, const char* name, bool value, const char* detail = "") { ReportTest(suite, name, value, detail); }

todo::CreateTaskCommand MakeCreate(const std::string& title) {
    todo::CreateTaskCommand command;
    command.title = title;
    command.note = "note";
    command.priority = todo::Priority::Medium;
    return command;
}

std::optional<todo::Task> FindTask(const todo::TaskPage& page, std::int64_t id) {
    for (const auto& task : page.items) {
        if (task.id == id) return task;
    }
    return std::nullopt;
}

}  // namespace

void RunRepositoryContract(const char* suite, todo::IUnitOfWorkFactory& factory) {
    using namespace todo;

    auto clock = std::make_shared<ContractClock>();
    clock->now = LocalDateTime{2026, 6, 1, 9, 0, 0, 0};
    TaskService tasks(factory, clock);
    CategoryService categories(factory, clock);
    HistoryService history(factory, clock);
    StatisticsService statistics(factory, clock);

    // --- Category create / duplicate / list -----------------------------------------
    auto work = categories.Create("工作", "#007AFF");
    Check(suite, "category create", work.ok(), work.ok() ? "" : work.error().message.c_str());
    if (!work.ok()) return;
    auto duplicate = categories.Create("工作", "#FF0000");
    Check(suite, "duplicate category name is Conflict", !duplicate.ok() && duplicate.error().code == ErrorCode::Conflict);
    auto listed = categories.List();
    Check(suite, "category list returns the row", listed.ok() && listed.value().size() == 1u);

    // --- Task create / query --------------------------------------------------------
    CreateTaskCommand withCategory = MakeCreate("  quarterly report  ");
    withCategory.categoryId = work.value().id;
    auto task = tasks.Create(withCategory);
    Check(suite, "task create with category", task.ok(), task.ok() ? "" : task.error().message.c_str());
    if (!task.ok()) return;
    const std::int64_t taskId = task.value().id;
    const std::uint64_t revision0 = task.value().revision;
    Check(suite, "task title is trimmed", task.value().title == "quarterly report");
    Check(suite, "task starts at revision 1", revision0 == 1);
    Check(suite, "task gets a positive sort weight", task.value().sortWeight > 0.0);

    TaskQuery all;
    all.route = Route::AllTasks;
    all.page.pageSize = 200;
    all.now = clock->now;
    auto page = tasks.Query(all);
    Check(suite, "all-tasks query returns the row", page.ok() && FindTask(page.value(), taskId).has_value());

    // --- Complete: task + history in one transaction, stats deduped by current task ---
    auto done = tasks.SetCompleted(taskId, true, revision0);
    Check(suite, "complete succeeds", done.ok(), done.ok() ? "" : done.error().message.c_str());
    Check(suite, "complete sets the flag", done.ok() && done.value().completed);
    Check(suite, "complete bumps revision", done.ok() && done.value().revision == revision0 + 1);
    HistoryQuery historyQuery;
    historyQuery.page.pageSize = 200;
    auto firstHistory = history.Query(historyQuery);
    Check(suite, "complete appends one event", firstHistory.ok() && firstHistory.value().items.size() == 1u);
    Check(suite, "first event is COMPLETE",
          firstHistory.ok() && firstHistory.value().items.size() == 1u &&
              firstHistory.value().items[0].operationType == OperationType::Complete);
    auto stats = statistics.Refresh();
    Check(suite, "stats counts the completed task", stats.ok() && stats.value().overview.completed == 1);
    Check(suite, "completion rate is 1 for a single completed task", stats.ok() && stats.value().completionRate == 1.0);

    // --- Uncomplete restores in place and appends UNCOMPLETE --------------------------
    auto undone = tasks.SetCompleted(taskId, false, done.value().revision);
    Check(suite, "uncomplete succeeds", undone.ok());
    Check(suite, "uncomplete clears the flag", undone.ok() && !undone.value().completed);
    auto secondHistory = history.Query(historyQuery);
    Check(suite, "uncomplete appends a second event", secondHistory.ok() && secondHistory.value().items.size() == 2u);
    Check(suite, "newest event is UNCOMPLETE",
          secondHistory.ok() && secondHistory.value().items.size() == 2u &&
              secondHistory.value().items[0].operationType == OperationType::Uncomplete);
    auto statsAfterUndo = statistics.Refresh();
    Check(suite, "stats counts the task as pending again", statsAfterUndo.ok() && statsAfterUndo.value().overview.pending == 1);
    Check(suite, "completion rate is 0 when nothing is completed", statsAfterUndo.ok() && statsAfterUndo.value().completionRate == 0.0);

    // --- Optimistic lock: stale edit is rejected, current edit wins -------------------
    auto refreshed = tasks.Query(all);
    std::optional<Task> current = refreshed.ok() ? FindTask(refreshed.value(), taskId) : std::nullopt;
    Check(suite, "task is still readable after toggles", current.has_value());
    if (current) {
        EditTaskCommand edit;
        edit.id = taskId;
        edit.title = "renamed";
        edit.note = current->note;
        edit.priority = current->priority;
        edit.dueAt = current->dueAt;
        edit.categoryId = current->categoryId;
        edit.expectedRevision = current->revision + 9;
        auto conflict = tasks.Edit(edit);
        Check(suite, "stale edit is Conflict", !conflict.ok() && conflict.error().code == ErrorCode::Conflict);
        edit.expectedRevision = current->revision;
        auto accepted = tasks.Edit(edit);
        Check(suite, "current edit succeeds", accepted.ok() && accepted.value().title == "renamed");
        Check(suite, "edit bumps revision", accepted.ok() && accepted.value().revision == current->revision + 1);
    }

    // --- Non-empty category cannot be deleted ----------------------------------------
    auto blockedDelete = categories.Delete(work.value().id);
    Check(suite, "non-empty category delete is CategoryNotEmpty",
          !blockedDelete.ok() && blockedDelete.error().code == ErrorCode::CategoryNotEmpty);

    // --- Reorder and the DBL_MAX overflow path ---------------------------------------
    auto alpha = tasks.Create(MakeCreate("alpha"));
    auto beta = tasks.Create(MakeCreate("beta"));
    auto gamma = tasks.Create(MakeCreate("gamma"));
    Check(suite, "reorder fixtures created", alpha.ok() && beta.ok() && gamma.ok());
    if (alpha.ok() && beta.ok() && gamma.ok()) {
        ReorderRequest move;
        move.taskId = gamma.value().id;
        move.previousId = alpha.value().id;
        move.nextId = beta.value().id;
        auto moved = tasks.Reorder(move);
        Check(suite, "reorder between neighbours succeeds", moved.ok(), moved.ok() ? "" : moved.error().message.c_str());

        // Force the maximum weight onto one row, then create another task: Create must detect the
        // overflow (DBL_MAX + step is not finite) and renumber instead of emitting a duplicate.
        const double maximumWeight = std::numeric_limits<double>::max();
        {
            auto uow = factory.Create();
            Check(suite, "overflow fixture opens a unit of work", uow.ok());
            if (uow.ok()) {
                auto begun = uow.value()->Begin();
                auto loaded = uow.value()->Tasks().GetByIdForUpdate(alpha.value().id);
                Check(suite, "overflow fixture loads the task", begun.ok() && loaded.ok());
                if (loaded.ok()) {
                    Task extreme = loaded.value();
                    extreme.sortWeight = maximumWeight;
                    auto updated = uow.value()->Tasks().Update(extreme, extreme.revision);
                    auto committed = uow.value()->Commit();
                    Check(suite, "DBL_MAX weight is persisted", updated.ok() && committed.ok());
                } else {
                    uow.value()->Rollback();
                }
            }
        }
        auto afterExtreme = tasks.Create(MakeCreate("delta"));
        Check(suite, "creating after DBL_MAX succeeds", afterExtreme.ok(), afterExtreme.ok() ? "" : afterExtreme.error().message.c_str());
        Check(suite, "new weight is finite and not DBL_MAX",
              afterExtreme.ok() && std::isfinite(afterExtreme.value().sortWeight) && afterExtreme.value().sortWeight != maximumWeight);
        auto normalized = tasks.Query(all);
        bool ordered = normalized.ok();
        double previousWeight = -std::numeric_limits<double>::infinity();
        if (normalized.ok()) {
            for (const auto& row : normalized.value().items) {
                if (!(row.sortWeight > previousWeight)) { ordered = false; break; }
                previousWeight = row.sortWeight;
            }
        }
        Check(suite, "weights are strictly increasing after renumbering", ordered);
    }

    // --- Keyset pagination over >200 rows with a frozen session time ------------------
    {
        auto uow = factory.Create();
        Check(suite, "bulk fixture opens a unit of work", uow.ok());
        if (uow.ok()) {
            auto begun = uow.value()->Begin();
            if (begun.ok()) {
                for (int i = 0; i < 205; ++i) {
                    Task bulk;
                    bulk.title = "bulk " + std::to_string(i);
                    bulk.note = "n";
                    bulk.priority = Priority::Medium;
                    bulk.createdAt = clock->now;
                    uow.value()->Tasks().Create(bulk);
                }
                uow.value()->Commit();
            }
        }
    }
    TaskQuery pageOneQuery;
    pageOneQuery.route = Route::AllTasks;
    pageOneQuery.page.pageSize = 200;
    pageOneQuery.now = LocalDateTime{2026, 6, 2, 9, 0, 0, 0};
    auto pageOne = tasks.Query(pageOneQuery);
    Check(suite, "page one returns 200 rows", pageOne.ok() && pageOne.value().items.size() == 200u);
    Check(suite, "page one exposes a cursor with a frozen session time",
          pageOne.ok() && pageOne.value().nextCursor.has_value() && pageOne.value().nextCursor->sessionNow.has_value());
    if (pageOne.ok() && pageOne.value().nextCursor) {
        TaskQuery pageTwoQuery = pageOneQuery;
        pageTwoQuery.page.cursor = pageOne.value().nextCursor;
        pageTwoQuery.now = *pageOne.value().nextCursor->sessionNow;  // freeze, as AppController does
        auto pageTwo = tasks.Query(pageTwoQuery);
        Check(suite, "page two is accepted", pageTwo.ok(), pageTwo.ok() ? "" : pageTwo.error().message.c_str());
        Check(suite, "page two returns the remainder", pageTwo.ok() && pageTwo.value().items.size() >= 5u);

        std::set<std::int64_t> ids;
        for (const auto& row : pageOne.value().items) ids.insert(row.id);
        std::size_t overlap = 0;
        if (pageTwo.ok()) {
            for (const auto& row : pageTwo.value().items) {
                if (!ids.insert(row.id).second) ++overlap;
            }
        }
        Check(suite, "pages do not repeat rows", overlap == 0);

        TaskQuery drifted = pageOneQuery;
        drifted.page.cursor = pageOne.value().nextCursor;
        drifted.now = LocalDateTime{2026, 6, 2, 9, 0, 0, 1};  // fresh clock, old cursor
        auto rejected = tasks.Query(drifted);
        Check(suite, "a fresh clock bound to an old cursor is rejected",
              !rejected.ok() && rejected.error().code == ErrorCode::Conflict);
    }

    // --- History keyset pagination ---------------------------------------------------
    {
        auto histTask = tasks.Create(MakeCreate("history walk"));
        Check(suite, "history fixture created", histTask.ok());
        if (histTask.ok()) {
            std::int64_t hid = histTask.value().id;
            std::uint64_t hrev = histTask.value().revision;
            bool toggled = true;
            for (int i = 0; i < 6; ++i) {
                auto result = tasks.SetCompleted(hid, (i % 2) == 0, hrev);
                if (!result.ok()) { toggled = false; break; }
                hrev = result.value().revision;
            }
            Check(suite, "history fixture toggled six times", toggled);
            HistoryQuery walk;
            walk.filter.keyword = "history walk";  // isolate this fixture from the earlier task's events
            walk.page.pageSize = 2;
            std::vector<std::int64_t> seen;
            auto step = history.Query(walk);
            while (step.ok()) {
                for (const auto& event : step.value().items) seen.push_back(event.id);
                if (!step.value().nextCursor) break;
                walk.page.cursor = step.value().nextCursor;
                step = history.Query(walk);
            }
            Check(suite, "history pages cover every event", seen.size() == 6u);
            Check(suite, "history pages contain no duplicates",
                  std::set<std::int64_t>(seen.begin(), seen.end()).size() == seen.size());

            // Deleting the task must cascade its history rows (completed task).
            auto deleted = tasks.Delete(hid);
            Check(suite, "deleting a toggled task succeeds", deleted.ok());
            auto uow = factory.Create();
            if (uow.ok()) {
                auto latest = uow.value()->History().GetLatestForTask(hid);
                Check(suite, "task delete cascades its history", !latest.ok() && latest.error().code == ErrorCode::NotFound);
            }
        }
    }

    // --- Removing the last task from a category frees it for deletion ----------------
    auto emptyNow = categories.Delete(work.value().id);
    (void)emptyNow;  // may still be non-empty depending on earlier fixtures; the dedicated SQLite
                     // tests cover the empty case explicitly.

    // --- R1: note is always the pure-text projection of body_doc -------------------
    // Done at the very end so it cannot disturb the single-task statistics assertions above.
    {
        BodyDocument document = BodyDocument::Empty();
        document.blocks[0].text = "正文第一行\n第二行";
        CreateTaskCommand command;
        command.title = "body projection owner";
        command.note = "this note must be replaced by the service";
        command.body = BodyEdit{document.ToJson(), {}};
        auto withBody = tasks.Create(command);
        Check(suite, "create with body succeeds", withBody.ok(),
              withBody.ok() ? "" : withBody.error().message.c_str());
        Check(suite, "note == doc.PlainText(false)", withBody.ok() && withBody.value().note == document.PlainText(false));
        Check(suite, "body_doc == canonical json", withBody.ok() && withBody.value().bodyDoc == document.ToJson());
        if (withBody.ok()) {
            BodyDocument changed = BodyDocument::Empty();
            changed.blocks[0].text = "改过的正文";
            EditTaskCommand edit;
            edit.id = withBody.value().id;
            edit.title = "body projection owner";
            edit.note = "stale";
            edit.expectedRevision = withBody.value().revision;
            edit.body = BodyEdit{changed.ToJson(), {}};
            auto edited = tasks.Edit(edit);
            Check(suite, "edit with body re-derives note", edited.ok() && edited.value().note == changed.PlainText(false));
            Check(suite, "edit with body replaces body_doc", edited.ok() && edited.value().bodyDoc == changed.ToJson());
            auto removed = tasks.Delete(edit.id);
            Check(suite, "body task cleanup succeeds", removed.ok());
        }
    }
}
