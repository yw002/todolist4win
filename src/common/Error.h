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
    PermissionDenied,
    QueryFailed,
    SchemaUnsupported,
    Cancelled,
    // Body content (block document / pasted images).
    // BodySchemaUnsupported: body_doc has v>kBodyDocVersion or cannot be parsed. Reading the task
    // still succeeds; only saving is refused, so newer data is never overwritten.
    BodySchemaUnsupported,
    BodyTooLarge,      // JSON / block count / per-task image count or total bytes exceeded
    ImageTooLarge,     // single PNG bytes, dimension or pixel count
    ImageInvalid,      // bad PNG signature, undecodable bytes, key/bytes mismatch, length mismatch
    ImageUnavailable,  // Get() found no blob for this key (no retry, not a connection error)
    Internal
};

struct AppError {
    ErrorCode code{ErrorCode::None};
    std::string message;
    int nativeCode{0};

    [[nodiscard]] bool IsConnectionError() const noexcept {
        return code == ErrorCode::ConnectionFailed || code == ErrorCode::Timeout;
    }
};

}  // namespace todo
