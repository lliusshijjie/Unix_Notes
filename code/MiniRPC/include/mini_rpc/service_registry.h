#pragma once

#include "mini_rpc/protocol.h"

#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace minirpc {

using MethodHandler = std::function<void(const RpcRequest&, RpcResponse&)>;

enum class LookupStatus {
    Found,
    ServiceNotFound,
    MethodNotFound
};

struct LookupResult {
    LookupStatus status{LookupStatus::ServiceNotFound};
    MethodHandler handler;
};

class ServiceRegistry {
public:
    bool registerMethod(std::string serviceName, std::string methodName,
                        MethodHandler handler);
    LookupResult findMethod(std::string_view serviceName,
                            std::string_view methodName) const;

private:
    using MethodMap = std::unordered_map<std::string, MethodHandler>;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, MethodMap> services_;
};

}  // namespace minirpc
