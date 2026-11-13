#pragma once

#include <optional>
#include <string>
#include <utility>

#include "config/CredentialVault.h"

namespace todo {

// In-memory ICredentialVault used by the settings-service tests.
//
// It behaves exactly like the DPAPI implementation from the caller's point of view (nullopt vs
// empty string vs CredentialUnavailable) and can be told to fail a specific operation, which is the
// only practical way to exercise the "credential write failed, do not touch the INI" paths without
// manipulating real ACLs.
class FakeCredentialVault final : public ICredentialVault {
public:
    [[nodiscard]] Result<std::optional<std::string>> Load() override {
        ++loadCount;
        if (loadError.code != ErrorCode::None) return Result<std::optional<std::string>>::Failure(loadError);
        if (!value.has_value()) return Result<std::optional<std::string>>::Success(std::nullopt);
        return Result<std::optional<std::string>>::Success(value);
    }

    [[nodiscard]] Result<void> Save(const std::string& password) override {
        ++saveCount;
        if (saveError.code != ErrorCode::None) return Result<void>::Failure(saveError);
        value = password;
        return Result<void>::Success();
    }

    [[nodiscard]] Result<void> Remove() override {
        ++removeCount;
        if (removeError.code != ErrorCode::None) return Result<void>::Failure(removeError);
        value.reset();
        return Result<void>::Success();
    }

    [[nodiscard]] Result<bool> Exists() const override {
        if (existsError.code != ErrorCode::None) return Result<bool>::Failure(existsError);
        return Result<bool>::Success(value.has_value());
    }

    void FailSave(ErrorCode code, const char* message = "凭据写入失败") {
        saveError = AppError{code, message, 0, {}};
    }
    void FailLoad(ErrorCode code, const char* message = "凭据无法解密") {
        loadError = AppError{code, message, 0, {}};
    }
    void FailRemove(ErrorCode code, const char* message = "凭据删除失败") {
        removeError = AppError{code, message, 0, {}};
    }
    void ClearFailures() {
        saveError = AppError{};
        loadError = AppError{};
        removeError = AppError{};
        existsError = AppError{};
    }

    std::optional<std::string> value;
    int loadCount{0};
    int saveCount{0};
    int removeCount{0};
    AppError saveError{};
    AppError loadError{};
    AppError removeError{};
    AppError existsError{};
};

}  // namespace todo
