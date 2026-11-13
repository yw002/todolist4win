#include "domain/TaskRules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace todo::TaskRules {
namespace {
Result<void> Invalid(std::string message) {
    return Result<void>::Failure({ErrorCode::InvalidInput, std::move(message)});
}

bool IsSpace(unsigned char c) { return std::isspace(c) != 0; }
}  // namespace

std::string Trim(const std::string& value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](char c) { return IsSpace(static_cast<unsigned char>(c)); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](char c) { return IsSpace(static_cast<unsigned char>(c)); }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool IsValidUtf8(const std::string& value) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = p[i++];
        if (c <= 0x7F) continue;
        int continuation = 0;
        unsigned int code = 0;
        if ((c & 0xE0) == 0xC0) { continuation = 1; code = c & 0x1F; if (code < 2) return false; }
        else if ((c & 0xF0) == 0xE0) { continuation = 2; code = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { continuation = 3; code = c & 0x07; if (code > 4) return false; }
        else return false;
        if (i + static_cast<std::size_t>(continuation) > value.size()) return false;
        for (int n = 0; n < continuation; ++n) {
            const unsigned char d = p[i++];
            if ((d & 0xC0) != 0x80) return false;
            code = (code << 6) | (d & 0x3F);
        }
        if ((continuation == 2 && code < 0x800) || (continuation == 3 && code < 0x10000)) return false;
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
}

std::size_t Utf8Length(const std::string& value) noexcept {
    if (!IsValidUtf8(value)) return value.size();
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

Result<void> ValidateTitle(const std::string& title) {
    if (!IsValidUtf8(title)) return Invalid("标题包含无效字符编码");
    const std::string trimmed = Trim(title);
    if (trimmed.empty()) return Invalid("任务标题不能为空");
    if (Utf8Length(trimmed) > 500) return Invalid("任务标题不能超过 500 个字符");
    return Result<void>::Success();
}

Result<void> ValidateNote(const std::string& note) {
    if (!IsValidUtf8(note)) return Invalid("备注包含无效字符编码");
    if (note.size() > 60000) return Invalid("备注内容过长");
    return Result<void>::Success();
}

Result<void> ValidateCategoryName(const std::string& name) {
    if (!IsValidUtf8(name)) return Invalid("分类名称包含无效字符编码");
    const std::string trimmed = Trim(name);
    if (trimmed.empty()) return Invalid("分类名称不能为空");
    if (Utf8Length(trimmed) > 80) return Invalid("分类名称不能超过 80 个字符");
    return Result<void>::Success();
}

Result<void> ValidateColor(const std::string& color) {
    if (color.size() != 7 || color[0] != '#') return Invalid("颜色必须使用 #RRGGBB 格式");
    for (std::size_t i = 1; i < color.size(); ++i) {
        if (std::isxdigit(static_cast<unsigned char>(color[i])) == 0) return Invalid("颜色必须使用 #RRGGBB 格式");
    }
    return Result<void>::Success();
}

Result<void> ValidateKeyword(const std::string& keyword) {
    if (!IsValidUtf8(keyword)) return Invalid("搜索关键词包含无效字符编码");
    if (Utf8Length(keyword) > 200) return Invalid("搜索关键词不能超过 200 个字符");
    return Result<void>::Success();
}

Result<void> ValidateId(std::int64_t id) {
    if (id <= 0) return Invalid("记录标识无效");
    return Result<void>::Success();
}

Result<void> ValidateTask(const Task& task) {
    auto title = ValidateTitle(task.title);
    if (!title.ok()) return title;
    auto note = ValidateNote(task.note);
    if (!note.ok()) return note;
    if (task.categoryId) {
        auto id = ValidateId(*task.categoryId);
        if (!id.ok()) return id;
    }
    if (task.completed != task.completedAt.has_value()) return Invalid("完成状态与完成时间不一致");
    if (!std::isfinite(task.sortWeight)) return Invalid("排序权重无效");
    return Result<void>::Success();
}

std::string EscapeLike(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '%' || c == '_') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

}  // namespace todo::TaskRules
