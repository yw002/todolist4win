#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "FakeUnitOfWork.h"
#include "common/Error.h"
#include "common/Result.h"
#include "core/TaskService.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") {
    ReportTest("TaskService", name, value, detail);
}

LocalDateTime At(int year, int month, int day, int hour, int minute, int second, int microsecond) {
    return LocalDateTime{year, month, day, hour, minute, second, microsecond};
}

// One isolated database + service per scenario so call counters and history rows never bleed
// between test cases.
struct Fixture {
    test::FakeUnitOfWorkFactory factory;
    std::shared_ptr<test::FakeClock> clock{std::make_shared<test::FakeClock>()};
    TaskService service{factory, clock};
    test::FakeDatabase& db{factory.Db()};

    Fixture() { clock->now = At(2026, 5, 1, 9, 0, 0, 0); }

    Task SeedTask(std::optional<std::int64_t> categoryId = std::nullopt) {
        Task seed;
        seed.title = "seeded task";
        seed.categoryId = categoryId;
        auto created = db.tasks.Create(seed);
        return created.value();
    }

    void SeedCategory(std::int64_t id, const std::string& name, const std::string& color) {
        Category category;
        category.id = id;
        category.name = name;
        category.color = color;
        db.categories.rows[id] = category;
    }
};

}  // namespace

void RunTaskServiceTests() {
    const LocalDateTime t0 = At(2026, 5, 1, 9, 0, 0, 0);
    const LocalDateTime t1 = At(2026, 5, 2, 10, 30, 0, 500000);

    // --- Completing a task keeps the row and appends COMPLETE in one transaction ------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        const int commitBefore = f.db.commitCount;
        const int rollbackBefore = f.db.rollbackCount;

        const auto done = f.service.SetCompleted(seed.id, true, seed.revision);
        Check("complete returns success", done.ok());
        Check("complete keeps the same task row", done.ok() && done.value().id == seed.id);
        Check("complete sets the completed flag", done.ok() && done.value().completed);
        Check("complete stamps completedAt from the clock",
              done.ok() && done.value().completedAt.has_value() && *done.value().completedAt == t0);
        Check("complete bumps the revision", done.ok() && done.value().revision == seed.revision + 1);
        Check("complete persists the new state", f.db.tasks.GetByIdForUpdate(seed.id).value().completed);
        Check("complete appends exactly one history event", f.db.history.events.size() == 1u);
        Check("history event is COMPLETE",
              f.db.history.events.size() == 1u && f.db.history.events[0].operationType == OperationType::Complete);
        Check("history event points at the task", f.db.history.events.size() == 1u && f.db.history.events[0].taskId == seed.id);
        Check("history event uses the clock time",
              f.db.history.events.size() == 1u && f.db.history.events[0].eventAt == t0);
        Check("history snapshot keeps the original title",
              f.db.history.events.size() == 1u && f.db.history.events[0].snapshot.title == seed.title);
        Check("history snapshot keeps the completion time",
              f.db.history.events.size() == 1u && f.db.history.events[0].completedAt.has_value() &&
                  *f.db.history.events[0].completedAt == t0);
        Check("complete commits exactly once", f.db.commitCount == commitBefore + 1);
        Check("complete never rolls back", f.db.rollbackCount == rollbackBefore);
    }

    // --- Un-completing restores the row in place and appends UNCOMPLETE ---------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        const auto done = f.service.SetCompleted(seed.id, true, seed.revision);
        f.clock->now = t1;

        const auto undone = f.service.SetCompleted(seed.id, false, done.value().revision);
        Check("uncomplete returns success", undone.ok());
        Check("uncomplete clears the completed flag", undone.ok() && !undone.value().completed);
        Check("uncomplete clears completedAt", undone.ok() && !undone.value().completedAt.has_value());
        Check("uncomplete restores the same row instead of creating a new task",
              undone.ok() && undone.value().id == seed.id && f.db.tasks.rows.size() == 1u);
        Check("uncomplete appends a second event", f.db.history.events.size() == 2u);
        Check("second event is UNCOMPLETE",
              f.db.history.events.size() == 2u && f.db.history.events[1].operationType == OperationType::Uncomplete);
        Check("second event uses the later clock value", f.db.history.events.size() == 2u && f.db.history.events[1].eventAt == t1);
        Check("uncomplete commits a second transaction", f.db.commitCount == 2);
        Check("uncomplete appends no snapshot completion time",
              f.db.history.events.size() == 2u && !f.db.history.events[1].completedAt.has_value());
    }

    // --- A stale revision must be rejected before anything is written -----------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        const int commitBefore = f.db.commitCount;
        const int rollbackBefore = f.db.rollbackCount;

        const auto conflict = f.service.SetCompleted(seed.id, true, seed.revision + 99);
        Check("stale revision is a Conflict", !conflict.ok() && conflict.error().code == ErrorCode::Conflict);
        Check("stale revision writes no history", f.db.history.events.empty());
        Check("stale revision commits nothing", f.db.commitCount == commitBefore);
        Check("stale revision rolls the transaction back", f.db.rollbackCount == rollbackBefore + 1);
        const auto stored = f.db.tasks.GetByIdForUpdate(seed.id);
        Check("stale revision leaves the row untouched",
              stored.ok() && !stored.value().completed && stored.value().revision == seed.revision);
    }

    // --- Repeating the same transition is idempotent and writes nothing --------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        const auto first = f.service.SetCompleted(seed.id, true, seed.revision);
        const int commitAfterFirst = f.db.commitCount;

        const auto again = f.service.SetCompleted(seed.id, true, first.value().revision);
        Check("repeat complete still succeeds", again.ok() && again.value().completed);
        Check("repeat complete keeps the revision", again.ok() && again.value().revision == first.value().revision);
        Check("repeat complete writes no extra history", f.db.history.events.size() == 1u);
        Check("repeat complete opens no write transaction", f.db.commitCount == commitAfterFirst);
    }

    // --- History append failure must roll the whole completion back ------------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        f.db.history.failAppend = true;
        const int commitBefore = f.db.commitCount;

        const auto failed = f.service.SetCompleted(seed.id, true, seed.revision);
        Check("history failure fails the completion", !failed.ok());
        Check("history failure commits nothing", f.db.commitCount == commitBefore);
        Check("history failure writes no history", f.db.history.events.empty());
        const auto stored = f.db.tasks.GetByIdForUpdate(seed.id);
        Check("history failure rolls the task row back to pending",
              stored.ok() && !stored.value().completed && stored.value().revision == seed.revision);
    }

    // --- Commit failure must surface as an error and roll back -----------------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        f.db.failCommit = true;

        const auto failed = f.service.SetCompleted(seed.id, true, seed.revision);
        Check("commit failure surfaces as an error", !failed.ok());
        Check("commit failure writes no history", f.db.history.events.empty());
        const auto stored = f.db.tasks.GetByIdForUpdate(seed.id);
        Check("commit failure rolls the task back to pending", stored.ok() && !stored.value().completed);
    }

    // --- The history snapshot captures the original category -------------------------
    {
        Fixture f;
        f.SeedCategory(7, "工作", "#007AFF");
        const Task seed = f.SeedTask(7);

        const auto done = f.service.SetCompleted(seed.id, true, seed.revision);
        Check("completion with a category succeeds", done.ok());
        Check("snapshot captures the category id",
              f.db.history.events.size() == 1u && f.db.history.events[0].snapshot.originalCategoryId.has_value() &&
                  *f.db.history.events[0].snapshot.originalCategoryId == 7);
        Check("snapshot captures the category name",
              f.db.history.events.size() == 1u && f.db.history.events[0].snapshot.originalCategoryName == "工作");
        Check("snapshot captures the category colour",
              f.db.history.events.size() == 1u && f.db.history.events[0].snapshot.originalCategoryColor == "#007AFF");
    }

    // --- Create trims the title and stamps createdAt from the clock ------------------
    {
        Fixture f;
        CreateTaskCommand command;
        command.title = "   quarterly report   ";
        command.note = "draft";
        command.priority = Priority::High;

        const auto created = f.service.Create(command);
        Check("create returns success", created.ok());
        Check("create trims the title", created.ok() && created.value().title == "quarterly report");
        Check("create keeps the note", created.ok() && created.value().note == "draft");
        Check("create keeps the priority", created.ok() && created.value().priority == Priority::High);
        Check("create stamps createdAt from the clock", created.ok() && created.value().createdAt == f.clock->now);
        Check("create starts at revision 1", created.ok() && created.value().revision == 1);
        Check("create commits exactly once", f.db.commitCount == 1);
    }

    // --- Invalid input must be rejected before any unit of work is opened ------------
    {
        Fixture f;
        CreateTaskCommand command;
        command.title = "   ";
        const int createBefore = f.db.createCount;

        const auto rejected = f.service.Create(command);
        Check("blank title is InvalidInput", !rejected.ok() && rejected.error().code == ErrorCode::InvalidInput);
        Check("blank title opens no unit of work", f.db.createCount == createBefore);
        Check("blank title persists nothing", f.db.tasks.rows.empty());
        Check("blank title commits nothing", f.db.commitCount == 0);
    }

    // --- Delete removes the task row but must not touch history ---------------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        const auto done = f.service.SetCompleted(seed.id, true, seed.revision);
        Check("seed completion succeeded", done.ok());
        const std::size_t historyBefore = f.db.history.events.size();

        const auto deleted = f.service.Delete(seed.id);
        Check("delete returns success", deleted.ok());
        Check("delete keeps the history rows", f.db.history.events.size() == historyBefore);
        const auto stored = f.db.tasks.GetByIdForUpdate(seed.id);
        Check("delete removes the task row", !stored.ok() && stored.error().code == ErrorCode::NotFound);
    }

    // --- Edit enforces the optimistic lock ------------------------------------------
    {
        Fixture f;
        const Task seed = f.SeedTask();
        EditTaskCommand command;
        command.id = seed.id;
        command.title = "renamed";
        command.expectedRevision = seed.revision + 5;

        const auto conflict = f.service.Edit(command);
        Check("edit with a stale revision is a Conflict",
              !conflict.ok() && conflict.error().code == ErrorCode::Conflict);
        Check("edit conflict commits nothing", f.db.commitCount == 0);

        command.expectedRevision = seed.revision;
        const auto ok = f.service.Edit(command);
        Check("edit with the current revision succeeds", ok.ok() && ok.value().title == "renamed");
        Check("edit bumps the revision", ok.ok() && ok.value().revision == seed.revision + 1);
    }

    // --- Query validates its bounds before touching the repository ------------------
    {
        Fixture f;
        TaskQuery tooLarge;
        tooLarge.page.pageSize = 201;
        const auto rejectedSize = f.service.Query(tooLarge);
        Check("page size above 200 is rejected",
              !rejectedSize.ok() && rejectedSize.error().code == ErrorCode::InvalidInput);

        TaskQuery atLimit;
        atLimit.page.pageSize = 200;
        Check("page size 200 is accepted", f.service.Query(atLimit).ok());

        TaskQuery longKeyword;
        longKeyword.page.pageSize = 200;
        longKeyword.filter.keyword = std::string(201, 'x');
        const auto rejectedKeyword = f.service.Query(longKeyword);
        Check("keyword above 200 characters is rejected",
              !rejectedKeyword.ok() && rejectedKeyword.error().code == ErrorCode::InvalidInput);
    }
}
