#pragma once

#include <string>

namespace todo {

enum class ErrorCode {
    None,
    InvalidInput,
    NotFound,
    Conflict,
    CategoryNotEmpty,
    ConnectionFailed,
    Timeout,
    AuthenticationFailed,
    PermissionDenied,
    QueryFailed,
    SchemaUnsupported,
    Cancelled,
    // The stored credential file exists but cannot be turned back into a password: wrong user or
    // machine, corrupted/truncated blob, or an unknown container version. Callers must treat it as
    // "a secret exists that we cannot read", never as an empty password.
    CredentialUnavailable,
    Internal
};

struct AppError {
    ErrorCode code{ErrorCode::None};
    std::string message;
    int nativeCode{0};
    std::string sqlState;

    [[nodiscard]] bool IsConnectionError() const noexcept {
        return code == ErrorCode::ConnectionFailed || code == ErrorCode::Timeout;
    }
};

}  // namespace todo
