#include "common/Guid.h"

#include <rpc.h>
#include <string>
#include <windows.h>

namespace todo {

std::string NewGuidString() {
    UUID id{};
    if (UuidCreate(&id) != RPC_S_OK) return {};
    RPC_CSTR text = nullptr;
    if (UuidToStringA(&id, &text) != RPC_S_OK || text == nullptr) return {};
    std::string result(reinterpret_cast<char*>(text));
    RpcStringFreeA(&text);
    return result;
}

}  // namespace todo
