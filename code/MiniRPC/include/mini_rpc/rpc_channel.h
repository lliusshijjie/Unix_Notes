#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace minirpc {

class RpcClient;
class RpcController;

class RpcChannel {
public:
    explicit RpcChannel(std::shared_ptr<RpcClient> client);

    bool callMethod(std::string_view serviceName, std::string_view methodName,
                    std::string requestPayload, std::string& responsePayload,
                    RpcController& controller);

private:
    std::shared_ptr<RpcClient> client_;
};

}  // namespace minirpc
