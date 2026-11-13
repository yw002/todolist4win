#include <cstdlib>
#include <memory>
#include <string>

#include "common/Clock.h"

void ReportTest(const char*, const char*, bool, const char*);
void ReportSkipped(const char*, const char*, const char*);

// These tests are opt-in: they are compiled out unless the project property
// TodoEnableMySqlIntegrationTests=true is passed, and even then they refuse to
// run without an isolated database name that ends in _test and comes from the
// environment. Nothing here is executed by a default build.
#if defined(TODO_ENABLE_MYSQL_INTEGRATION_TESTS)
#include "core/CategoryService.h"
#include "core/HistoryService.h"
#include "core/TaskService.h"
#include "data/MySqlUnitOfWork.h"

void RunMySqlIntegrationTests() {
    const char* database = std::getenv("TODO_MYSQL_TEST_DATABASE");
    if (!database || std::string(database).find("_test") == std::string::npos || std::string(database) == "todolist_db") {
        ReportSkipped("MySqlIntegration", "isolated database", "Set TODO_MYSQL_TEST_DATABASE to a dedicated name containing _test.");
        return;
    }
    todo::DatabaseConfig config;
    config.host = std::getenv("TODO_MYSQL_TEST_HOST") ? std::getenv("TODO_MYSQL_TEST_HOST") : "localhost";
    config.username = std::getenv("TODO_MYSQL_TEST_USER") ? std::getenv("TODO_MYSQL_TEST_USER") : "root";
    // Never hard-code credentials here; the password must come from the environment.
    config.password = std::getenv("TODO_MYSQL_TEST_PASSWORD") ? std::getenv("TODO_MYSQL_TEST_PASSWORD") : "";
    config.database = database;
    try {
        todo::MySqlUnitOfWorkFactory factory(config);
        auto schema = factory.EnsureDatabaseAndSchema();
        ReportTest("MySqlIntegration", "idempotent schema", schema.ok(), schema.ok() ? "" : schema.error().message.c_str());
        if (!schema.ok()) return;
        auto clock = std::make_shared<todo::SystemClock>();
        todo::CategoryService categories(factory, clock);
        auto category = categories.Create("Integration Test Category", "#007AFF");
        ReportTest("MySqlIntegration", "category create", category.ok(), "");
        if (!category.ok()) return;
        todo::TaskService tasks(factory, clock);
        todo::CreateTaskCommand create;
        create.title = "Integration task";
        create.categoryId = category.value().id;
        auto task = tasks.Create(create);
        ReportTest("MySqlIntegration", "task create", task.ok(), "");
        if (task.ok()) {
            auto complete = tasks.SetCompleted(task.value().id, true, task.value().revision);
            ReportTest("MySqlIntegration", "complete with history transaction", complete.ok(), "");
            if (complete.ok()) {
                todo::HistoryService history(factory, clock);
                todo::HistoryQuery query;
                query.filter.categoryId = category.value().id;
                auto events = history.Query(query);
                ReportTest("MySqlIntegration", "complete event snapshot present", events.ok() && !events.value().items.empty(), "");
                auto restored = history.Restore(task.value().id);
                ReportTest("MySqlIntegration", "restore original task without duplication", restored.ok() && restored.value().id == task.value().id, "");
                tasks.Delete(task.value().id);
            }
        }
        auto removed = categories.Delete(category.value().id);
        ReportTest("MySqlIntegration", "category cleanup", removed.ok(), "");
    } catch (const std::exception&) {
        ReportTest("MySqlIntegration", "unexpected exception contained", false, "Integration test caught an unexpected exception.");
    }
}
#else
void RunMySqlIntegrationTests() {
    ReportSkipped("MySqlIntegration", "database integration", "Rebuild with TODO_ENABLE_MYSQL_INTEGRATION_TESTS and an isolated *_test database to execute.");
}
#endif
