#include <cmath>
#include <limits>
#include <string>

#include "domain/TaskRules.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("TaskRules", name, value, detail); }

std::string Repeat(char value, std::size_t count) { return std::string(count, value); }
}  // namespace

void RunTaskRulesTests() {
    // --- Title --------------------------------------------------------------
    Check("empty title rejected", !TaskRules::ValidateTitle("   ").ok());
    Check("whitespace only title rejected", !TaskRules::ValidateTitle("\t\r\n ").ok());
    Check("valid UTF-8 Chinese title", TaskRules::ValidateTitle("\xE5\xAE\x8C\xE6\x88\x90\xE5\x91\xA8\xE6\x8A\xA5").ok());
    Check("invalid UTF-8 rejected", !TaskRules::ValidateTitle(std::string("\xC0\xAF", 2)).ok());
    Check("lone continuation byte rejected", !TaskRules::ValidateTitle(std::string("\x80", 1)).ok());
    Check("truncated UTF-8 rejected", !TaskRules::ValidateTitle(std::string("\xE4\xB8", 2)).ok());
    Check("overlong title rejected", !TaskRules::ValidateTitle(Repeat('a', 501)).ok());
    Check("title at 500 code points accepted", TaskRules::ValidateTitle(Repeat('a', 500)).ok());
    Check("title length counts code points not bytes",
          TaskRules::ValidateTitle(Repeat('a', 500)).ok() && TaskRules::Utf8Length(Repeat('a', 500)) == 500);
    Check("4 byte emoji title accepted", TaskRules::ValidateTitle("\xF0\x9F\x98\x80").ok());

    // --- Note / keyword / category name -------------------------------------
    Check("note at limit accepted", TaskRules::ValidateNote(Repeat('n', 60000)).ok());
    Check("over long note rejected", !TaskRules::ValidateNote(Repeat('n', 60001)).ok());
    Check("empty note accepted", TaskRules::ValidateNote("").ok());
    Check("category name at 80 accepted", TaskRules::ValidateCategoryName(Repeat('c', 80)).ok());
    Check("category name over 80 rejected", !TaskRules::ValidateCategoryName(Repeat('c', 81)).ok());
    Check("empty category name rejected", !TaskRules::ValidateCategoryName("  ").ok());
    Check("keyword at 200 accepted", TaskRules::ValidateKeyword(Repeat('k', 200)).ok());
    Check("keyword over 200 rejected", !TaskRules::ValidateKeyword(Repeat('k', 201)).ok());
    Check("empty keyword accepted", TaskRules::ValidateKeyword("").ok());
    Check("invalid keyword encoding rejected", !TaskRules::ValidateKeyword(std::string("\xFF", 1)).ok());

    // --- Ids ----------------------------------------------------------------
    Check("positive id accepted", TaskRules::ValidateId(1).ok());
    Check("zero id rejected", !TaskRules::ValidateId(0).ok());
    Check("negative id rejected", !TaskRules::ValidateId(-7).ok());

    // --- Colour -------------------------------------------------------------
    Check("valid color", TaskRules::ValidateColor("#12aBcF").ok());
    Check("bad color rejected", !TaskRules::ValidateColor("12aBcF").ok());
    Check("short color rejected", !TaskRules::ValidateColor("#12aBc").ok());
    Check("long color rejected", !TaskRules::ValidateColor("#12aBcF0").ok());
    Check("non hex color rejected", !TaskRules::ValidateColor("#GGGGGG").ok());

    // --- LIKE escaping / trimming ------------------------------------------
    Check("LIKE wildcard escaped", TaskRules::EscapeLike("a%b_c\\d") == "a\\%b\\_c\\\\d");
    Check("LIKE escaping of plain text unchanged", TaskRules::EscapeLike("plain") == "plain");
    Check("LIKE escaping of empty value", TaskRules::EscapeLike("").empty());
    Check("trim ASCII whitespace", TaskRules::Trim("  task \t") == "task");
    Check("trim leaves inner spaces", TaskRules::Trim("  two words  ") == "two words");
    Check("trim of whitespace only is empty", TaskRules::Trim("   \t\r\n").empty());

    // --- Encoding helpers ---------------------------------------------------
    Check("UTF-8 length counts code points", TaskRules::Utf8Length("\xE4\xB8\xAD\xE6\x96\x87" "Ab") == 4);
    Check("UTF-8 length counts emoji as one code point", TaskRules::Utf8Length("\xF0\x9F\x98\x80") == 1);
    Check("UTF-8 length falls back to byte size when invalid", TaskRules::Utf8Length(std::string("\x80", 1)) == 1);
    Check("surrogate rejected as UTF-8", !TaskRules::IsValidUtf8(std::string("\xED\xA0\x80", 3)));
    Check("plain ASCII is valid UTF-8", TaskRules::IsValidUtf8("plain-ascii"));
    Check("empty value is valid UTF-8", TaskRules::IsValidUtf8(""));

    // --- Whole task ---------------------------------------------------------
    Task task;
    task.title = "Valid";
    task.completed = false;
    task.sortWeight = 1024.0;
    Check("valid task", TaskRules::ValidateTask(task).ok());

    task.completedAt = LocalDateTime{2026, 1, 1};
    Check("completion timestamp mismatch rejected", !TaskRules::ValidateTask(task).ok());
    task.completedAt.reset();

    task.completed = true;
    Check("completed without timestamp rejected", !TaskRules::ValidateTask(task).ok());
    task.completedAt = LocalDateTime{2026, 1, 1};
    Check("completed with timestamp accepted", TaskRules::ValidateTask(task).ok());
    task.completed = false;
    task.completedAt.reset();

    task.categoryId = 0;
    Check("zero category id rejected", !TaskRules::ValidateTask(task).ok());
    task.categoryId = 3;
    Check("positive category id accepted", TaskRules::ValidateTask(task).ok());
    task.categoryId.reset();

    task.sortWeight = std::numeric_limits<double>::quiet_NaN();
    Check("nan sort weight rejected", !TaskRules::ValidateTask(task).ok());
    task.sortWeight = std::numeric_limits<double>::infinity();
    Check("infinite sort weight rejected", !TaskRules::ValidateTask(task).ok());
    task.sortWeight = -1024.0;
    Check("finite negative sort weight accepted", TaskRules::ValidateTask(task).ok());

    task.title.clear();
    Check("task without title rejected", !TaskRules::ValidateTask(task).ok());
}
