#include "mini_rpc/rpc_server.h"

#include "base/Logger.h"
#include "mini_rpc/rpc_controller.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <exception>
#include <utility>

namespace minirpc {

RpcServer::RpcServer(minireactor::EventLoop* loop,
                     const minireactor::InetAddress& address,
                     std::size_t ioThreadCount, std::size_t workerThreadCount,
                     std::size_t queueCapacity)
    : tcpServer_(loop, address.port(), address.ip().c_str(), ioThreadCount),
      businessPool_(workerThreadCount, queueCapacity, RejectPolicy::abort) {
    tcpServer_.setMessageCallback(
        [this](const std::shared_ptr<minireactor::TcpConnection>& connection,
               minireactor::Buffer* buffer) { onMessage(connection, buffer); });
}

bool RpcServer::registerMethod(std::string serviceName, std::string methodName,
                               MethodHandler handler) {
    return registry_.registerMethod(std::move(serviceName), std::move(methodName),
                                    std::move(handler));
}

bool RpcServer::registerService(google::protobuf::Service* service) {
    if (service == nullptr) {
        return false;
    }
    const google::protobuf::ServiceDescriptor* descriptor = service->GetDescriptor();
    if (descriptor == nullptr || descriptor->method_count() <= 0) {
        return false;
    }

    bool registered = true;
    for (int index = 0; index < descriptor->method_count(); ++index) {
        const google::protobuf::MethodDescriptor* method = descriptor->method(index);
        registered =
            registerMethod(
                descriptor->name(), method->name(),
                [service, method](const RpcRequest& request, RpcResponse& response) {
                    std::unique_ptr<google::protobuf::Message> pbRequest(
                        service->GetRequestPrototype(method).New());
                    std::unique_ptr<google::protobuf::Message> pbResponse(
                        service->GetResponsePrototype(method).New());
                    if (!pbRequest->ParseFromString(request.payload)) {
                        response.error_code = static_cast<int>(RpcErrorCode::ProtocolError);
                        response.error_message = "failed to parse protobuf request";
                        return;
                    }

                    RpcController controller;
                    service->CallMethod(method, &controller, pbRequest.get(),
                                        pbResponse.get(), nullptr);
                    if (controller.failed()) {
                        response.error_code =
                            controller.errorCode() != 0
                                ? controller.errorCode()
                                : static_cast<int>(RpcErrorCode::ServerError);
                        response.error_message = controller.errorText();
                        return;
                    }
                    if (!pbResponse->SerializeToString(&response.payload)) {
                        response.error_code = static_cast<int>(RpcErrorCode::ServerError);
                        response.error_message = "failed to serialize protobuf response";
                        response.payload.clear();
                    }
                }) &&
            registered;
    }
    return registered;
}

void RpcServer::start() {
    tcpServer_.start();
}

void RpcServer::onMessage(
    const std::shared_ptr<minireactor::TcpConnection>& connection,
    minireactor::Buffer* buffer) {
    for (;;) {
        RpcRequest request;
        const DecodeStatus status = codec_.tryDecodeRequest(*buffer, request);
        if (status == DecodeStatus::NeedMoreData) {
            return;
        }
        if (status == DecodeStatus::ProtocolError) {
            MR_LOG_WARN(connection->name() + " sent an invalid RPC request");
            connection->forceClose();
            return;
        }
        handleRequest(connection, std::move(request));
    }
}

void RpcServer::handleRequest(
    const std::shared_ptr<minireactor::TcpConnection>& connection,
    RpcRequest request) {
    const std::uint64_t requestId = request.request_id;
    const LookupResult lookup =
        registry_.findMethod(request.service_name, request.method_name);
    if (lookup.status == LookupStatus::ServiceNotFound) {
        sendError(connection, request.request_id, RpcErrorCode::ServiceNotFound,
                  "service not found: " + request.service_name);
        return;
    }
    if (lookup.status == LookupStatus::MethodNotFound) {
        sendError(connection, request.request_id, RpcErrorCode::MethodNotFound,
                  "method not found: " + request.service_name + "." +
                      request.method_name);
        return;
    }

    std::weak_ptr<minireactor::TcpConnection> weakConnection = connection;
    MethodHandler handler = lookup.handler;
    const SubmitResult result = businessPool_.try_post(
        [this, weakConnection, handler = std::move(handler),
         request = std::move(request)]() mutable {
            RpcResponse response;
            response.request_id = request.request_id;
            try {
                handler(request, response);
                response.request_id = request.request_id;
            } catch (const std::exception& error) {
                response.error_code = static_cast<int>(RpcErrorCode::ServerError);
                response.error_message = error.what();
                response.payload.clear();
            } catch (...) {
                response.error_code = static_cast<int>(RpcErrorCode::ServerError);
                response.error_message = "unknown server error";
                response.payload.clear();
            }

            if (std::shared_ptr<minireactor::TcpConnection> activeConnection =
                    weakConnection.lock()) {
                std::string frame;
                try {
                    frame = codec_.encodeResponse(response);
                } catch (const std::exception& error) {
                    response.error_code = static_cast<int>(RpcErrorCode::ServerError);
                    response.error_message =
                        std::string("failed to encode RPC response: ") + error.what();
                    response.payload.clear();
                    frame = codec_.encodeResponse(response);
                }
                activeConnection->send(std::move(frame));
            }
        });

    if (result != SubmitResult::accepted) {
        sendError(connection, requestId, RpcErrorCode::ServerError,
                  "server business queue is full");
    }
}

void RpcServer::sendError(
    const std::shared_ptr<minireactor::TcpConnection>& connection,
    std::uint64_t requestId, RpcErrorCode errorCode, std::string errorMessage) {
    RpcResponse response;
    response.request_id = requestId;
    response.error_code = static_cast<int>(errorCode);
    response.error_message = std::move(errorMessage);
    connection->send(codec_.encodeResponse(response));
}

}  // namespace minirpc
