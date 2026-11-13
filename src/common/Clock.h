#pragma once

#include <memory>

#include "domain/Models.h"

namespace todo {

class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual LocalDateTime NowLocal() const = 0;
};

class SystemClock final : public IClock {
public:
    [[nodiscard]] LocalDateTime NowLocal() const override;
};

}  // namespace todo
