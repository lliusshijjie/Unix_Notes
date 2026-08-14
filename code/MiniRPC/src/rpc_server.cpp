#include "mini_rpc/rpc_server.h"

#include "base/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"

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
                activeConnection->send(codec_.encodeResponse(response));
            }
        });

    if (result != SubmitResult::accepted) {
        sendError(connection, request.request_id, RpcErrorCode::ServerError,
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
