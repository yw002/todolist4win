#pragma once

#include <optional>
#include <string>

#include "common/Result.h"
#include "domain/Models.h"

namespace todo::TaskRules {

[[nodiscard]] Result<void> ValidateTitle(const std::string& title);
[[nodiscard]] Result<void> ValidateNote(const std::string& note);
[[nodiscard]] Result<void> ValidateCategoryName(const std::string& name);
[[nodiscard]] Result<void> ValidateColor(const std::string& color);
[[nodiscard]] Result<void> ValidateKeyword(const std::string& keyword);
[[nodiscard]] Result<void> ValidateId(std::int64_t id);
[[nodiscard]] Result<void> ValidateTask(const Task& task);
[[nodiscard]] std::string Trim(const std::string& value);
[[nodiscard]] std::string EscapeLike(const std::string& value);
[[nodiscard]] std::size_t Utf8Length(const std::string& value) noexcept;
[[nodiscard]] bool IsValidUtf8(const std::string& value) noexcept;

}  // namespace todo::TaskRules
