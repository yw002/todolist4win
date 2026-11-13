#pragma once

namespace todo {

class IUiDispatcher {
public:
    virtual ~IUiDispatcher() = default;
    virtual void PostCompletion() = 0;
};

}  // namespace todo
