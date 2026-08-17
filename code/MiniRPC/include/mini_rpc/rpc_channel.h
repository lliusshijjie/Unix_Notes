#pragma once

#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_call_options.h"
#include "mini_rpc/rpc_future.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace minirpc {

class RpcClient;
class RpcController;
using RpcCallback = std::function<void(RpcResponse)>;

class RpcChannel {
public:
    explicit RpcChannel(std::shared_ptr<RpcClient> client);

    bool callMethod(std::string_view serviceName, std::string_view methodName,
                    std::string requestPayload, std::string& responsePayload,
                    RpcController& controller);

    RpcFuture callMethodAsync(std::string_view serviceName, std::string_view methodName,
                              std::string requestPayload,
                              RpcCallOptions options = {});

    void callMethodAsync(std::string_view serviceName, std::string_view methodName,
                         std::string requestPayload, RpcCallback callback,
                         RpcCallOptions options = {});

private:
    std::shared_ptr<RpcClient> client_;
};

}  // namespace minirpc
