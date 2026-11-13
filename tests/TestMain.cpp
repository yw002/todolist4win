#include <iostream>
#include <string>

namespace {
int g_total = 0;
int g_failures = 0;
int g_skipped = 0;

void PrintLine(const std::string& line) { std::cout << line << '\n'; }
}  // namespace

// Aggregating reporter: every suite calls these two functions. Failures are
// counted so that the process exit code reflects the real result; otherwise a
// broken assertion could never fail a build.
void ReportTest(const char* suite, const char* name, bool passed, const char* detail) {
    ++g_total;
    if (!passed) ++g_failures;
    std::string line = passed ? "[PASS] " : "[FAIL] ";
    line += suite;
    line += " :: ";
    line += name;
    if (detail && *detail) {
        line += " :: ";
        line += detail;
    }
    PrintLine(line);
    if (!passed) std::cout.flush();
}

void ReportSkipped(const char* suite, const char* name, const char* reason) {
    ++g_skipped;
    std::string line = "[SKIP] ";
    line += suite;
    line += " :: ";
    line += name;
    if (reason && *reason) {
        line += " :: ";
        line += reason;
    }
    PrintLine(line);
}

void RunTaskRulesTests();
void RunSortWeightTests();
void RunStatisticsTests();
void RunTimeRangesTests();
void RunConfigStoreTests();
void RunCredentialVaultTests();
void RunSettingsServiceTests();
void RunDataSourceSwitchStateTests();
void RunUiStateTests();
void RunModelsTests();
void RunTaskPaginationTests();
void RunTaskServiceTests();
void RunHistoryPaginationTests();
void RunSqliteRepositoryTests();
void RunSqliteTransactionTests();
void RunMySqlIntegrationTests();

int main() {
    PrintLine("TodoListWin native tests (no MySQL connection unless integration tests are explicitly enabled)");
    RunTaskRulesTests();
    RunSortWeightTests();
    RunStatisticsTests();
    RunTimeRangesTests();
    RunModelsTests();
    RunTaskPaginationTests();
    RunTaskServiceTests();
    RunHistoryPaginationTests();
    RunSqliteRepositoryTests();
    RunSqliteTransactionTests();
    RunConfigStoreTests();
    RunCredentialVaultTests();
    RunSettingsServiceTests();
    RunDataSourceSwitchStateTests();
    RunUiStateTests();
    RunMySqlIntegrationTests();

    PrintLine("");
    PrintLine("=== SUMMARY: " + std::to_string(g_total) + " checks, " + std::to_string(g_total - g_failures) +
              " passed, " + std::to_string(g_failures) + " failed, " + std::to_string(g_skipped) + " skipped ===");
    if (g_total == 0) {
        PrintLine("[FAIL] no checks were executed");
        return 2;
    }
    return g_failures == 0 ? 0 : 1;
}
