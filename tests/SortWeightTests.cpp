#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "domain/SortWeight.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("SortWeight", name, value, detail); }
bool Near(double left, double right) { return std::abs(left - right) < 1e-9; }
}  // namespace

void RunSortWeightTests() {
    // --- Appending ----------------------------------------------------------
    Check("new task appends one step", Near(SortWeight::NewTaskWeight(2048.0), 3072.0));
    Check("new task from empty table", Near(SortWeight::NewTaskWeight(0.0), SortWeight::kStep));
    Check("non finite maximum falls back to one step", Near(SortWeight::NewTaskWeight(std::numeric_limits<double>::quiet_NaN()), SortWeight::kStep));
    Check("overflowing maximum falls back to one step", Near(SortWeight::NewTaskWeight(std::numeric_limits<double>::max()), SortWeight::kStep));

    // --- Midpoints ----------------------------------------------------------
    auto midpoint = SortWeight::Between(1024.0, 2048.0);
    Check("midpoint halves the gap", midpoint.ok() && Near(midpoint.value(), 1536.0));
    auto beforeFirst = SortWeight::Between(std::nullopt, 1024.0);
    Check("insert before first", beforeFirst.ok() && Near(beforeFirst.value(), 0.0));
    auto afterLast = SortWeight::Between(1024.0, std::nullopt);
    Check("insert after last", afterLast.ok() && Near(afterLast.value(), 2048.0));
    auto emptyTable = SortWeight::Between(std::nullopt, std::nullopt);
    Check("insert into empty table", emptyTable.ok() && Near(emptyTable.value(), SortWeight::kStep));

    // --- Rejections ---------------------------------------------------------
    Check("invalid reversed neighbours", !SortWeight::Between(2048.0, 1024.0).ok());
    Check("reversed neighbours report invalid input", SortWeight::Between(2048.0, 1024.0).error().code == ErrorCode::InvalidInput);
    Check("identical neighbours rejected", !SortWeight::Between(1024.0, 1024.0).ok());
    Check("non finite previous rejected", SortWeight::Between(std::numeric_limits<double>::quiet_NaN(), 2048.0).error().code == ErrorCode::Conflict);
    Check("non finite next rejected", SortWeight::Between(1024.0, std::numeric_limits<double>::infinity()).error().code == ErrorCode::Conflict);

    // --- Renumbering threshold ---------------------------------------------
    Check("wide gap needs no renumbering", !SortWeight::NeedsRenumbering(1024.0, 2048.0));
    Check("exhausted gap needs renumbering", SortWeight::NeedsRenumbering(1.0, 1.0 + 1e-12));
    Check("exhausted gap surfaces conflict", SortWeight::Between(1.0, 1.0 + 1e-12).error().code == ErrorCode::Conflict);

    // --- Renumber -----------------------------------------------------------
    std::vector<Task> tasks(3);
    tasks[0].id = 3; tasks[0].sortWeight = 50;
    tasks[1].id = 1; tasks[1].sortWeight = 10;
    tasks[2].id = 2; tasks[2].sortWeight = 30;
    auto renumbered = SortWeight::Renumber(tasks);
    Check("renumber success", renumbered.ok());
    Check("renumber keeps order by weight", tasks[0].id == 1 && tasks[1].id == 2 && tasks[2].id == 3);
    Check("renumber restores step spacing", Near(tasks[0].sortWeight, SortWeight::kStep) && Near(tasks[2].sortWeight, SortWeight::kStep * 3));

    std::vector<Task> tied(2);
    tied[0].id = 2; tied[0].sortWeight = 1024;
    tied[1].id = 1; tied[1].sortWeight = 1024;
    Check("renumber breaks ties by id", SortWeight::Renumber(tied).ok() && tied[0].id == 1 && tied[1].id == 2);

    std::vector<Task> none;
    Check("renumber of empty list succeeds", SortWeight::Renumber(none).ok());
}
