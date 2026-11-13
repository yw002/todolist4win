#include "common/Clock.h"

#include <windows.h>

namespace todo {

LocalDateTime SystemClock::NowLocal() const {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    return LocalDateTime{value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond, value.wMilliseconds * 1000};
}

}  // namespace todo
