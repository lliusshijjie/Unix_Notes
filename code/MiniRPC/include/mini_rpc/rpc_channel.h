#pragma once

#include "mini_rpc/rpc_controller.h"

#include <google/protobuf/service.h>

#include <memory>
#include <string>
#include <string_view>

namespace minirpc {

class RpcClient;

class RpcChannel : public google::protobuf::RpcChannel {
public:
    explicit RpcChannel(std::shared_ptr<RpcClient> client);

    bool callMethod(std::string_view serviceName, std::string_view methodName,
                    std::string requestPayload, std::string& responsePayload,
                    RpcController& controller);

    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

private:
    std::shared_ptr<RpcClient> client_;
};

}  // namespace minirpc
