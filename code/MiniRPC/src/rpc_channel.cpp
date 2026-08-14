#include "mini_rpc/rpc_channel.h"

#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_controller.h"

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

}  // namespace minirpc
