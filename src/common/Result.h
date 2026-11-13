#pragma once

#include <utility>

#include "common/Error.h"

namespace todo {

template <typename T>
class Result {
public:
    static Result Success(T value) { return Result(std::move(value), AppError{}); }
    static Result Failure(AppError error) { return Result(T{}, std::move(error)); }

    [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::None; }
    [[nodiscard]] const T& value() const noexcept { return value_; }
    [[nodiscard]] T& value() noexcept { return value_; }
    [[nodiscard]] const AppError& error() const noexcept { return error_; }
    T TakeValue() { return std::move(value_); }

private:
    Result(T value, AppError error) : value_(std::move(value)), error_(std::move(error)) {}
    T value_{};
    AppError error_{};
};

template <>
class Result<void> {
public:
    static Result Success() { return Result(AppError{}); }
    static Result Failure(AppError error) { return Result(std::move(error)); }
    [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::None; }
    [[nodiscard]] const AppError& error() const noexcept { return error_; }
private:
    explicit Result(AppError error) : error_(std::move(error)) {}
    AppError error_{};
};

template <typename T>
using OperationResult = Result<T>;

}  // namespace todo
