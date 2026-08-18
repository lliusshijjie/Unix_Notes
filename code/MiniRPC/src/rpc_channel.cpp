#include "mini_rpc/rpc_channel.h"

#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_client.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <stdexcept>
#include <utility>

namespace minirpc {

RpcChannel::RpcChannel(std::shared_ptr<RpcClient> client)
    : client_(std::move(client)) {
    if (!client_) {
        throw std::invalid_argument("RpcChannel requires an RpcClient");
    }
}

bool RpcChannel::callMethod(std::string_view serviceName,
                            std::string_view methodName,
                            std::string requestPayload,
                            std::string& responsePayload,
                            RpcController& controller) {
    controller.reset();
    responsePayload.clear();

    RpcRequest request;
    request.service_name.assign(serviceName.data(), serviceName.size());
    request.method_name.assign(methodName.data(), methodName.size());
    request.payload = std::move(requestPayload);

    RpcResponse response = client_->call(std::move(request), controller.timeout());
    if (response.error_code != 0) {
        controller.setFailed(response.error_code, std::move(response.error_message));
        return false;
    }
    responsePayload = std::move(response.payload);
    return true;
}

void RpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* controller,
                            const google::protobuf::Message* request,
                            google::protobuf::Message* response,
                            google::protobuf::Closure* done) {
    RpcController localController;
    RpcController* rpcController = dynamic_cast<RpcController*>(controller);
    if (rpcController == nullptr) {
        rpcController = &localController;
    }

    auto finish = [controller, rpcController, done] {
        if (controller != nullptr && controller != rpcController && rpcController->failed()) {
            controller->SetFailed(rpcController->errorText());
        }
        if (done != nullptr) {
            done->Run();
        }
    };

    if (method == nullptr || method->service() == nullptr || request == nullptr ||
        response == nullptr) {
        rpcController->setFailed(static_cast<int>(RpcErrorCode::ProtocolError),
                                 "invalid protobuf RPC call");
        finish();
        return;
    }

    std::string requestPayload;
    if (!request->SerializeToString(&requestPayload)) {
        rpcController->setFailed(static_cast<int>(RpcErrorCode::ProtocolError),
                                 "failed to serialize protobuf request");
        finish();
        return;
    }

    std::string responsePayload;
    if (!callMethod(method->service()->name(), method->name(), std::move(requestPayload),
                    responsePayload, *rpcController)) {
        finish();
        return;
    }
    if (!response->ParseFromString(responsePayload)) {
        rpcController->setFailed(static_cast<int>(RpcErrorCode::ProtocolError),
                                 "failed to parse protobuf response");
        finish();
        return;
    }
    finish();
}

}  // namespace minirpc
