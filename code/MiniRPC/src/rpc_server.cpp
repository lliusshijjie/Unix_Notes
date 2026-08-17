#include "mini_rpc/rpc_server.h"

#include "base/Logger.h"
#include "mini_rpc/rpc_serializer.h"
#include "mini_rpc/rpc_trace_context.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"

#include <chrono>
#include <exception>
#include <utility>

namespace minirpc {

namespace {

RpcTraceContext makeTraceContext(const RpcRequest& request) {
    RpcTraceContext context;
    context.trace_id = request.trace_id;
    context.request_id = request.request_id;
    context.service_name = request.service_name;
    context.method_name = request.method_name;
    return context;
}

std::string traceLogPrefix(const RpcTraceContext& context) {
    return "trace_id=" + context.trace_id +
        " request_id=" + std::to_string(context.request_id) +
        " method=" + context.service_name + "." + context.method_name;
}

}  // namespace

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

RpcMetricsSnapshot RpcServer::metrics() const {
    RpcMetricsSnapshot snapshot;
    snapshot.server_total_requests = totalRequests_.load(std::memory_order_relaxed);
    snapshot.server_success_responses = successResponses_.load(std::memory_order_relaxed);
    snapshot.server_failed_responses = failedResponses_.load(std::memory_order_relaxed);
    snapshot.server_timeout_responses = timeoutResponses_.load(std::memory_order_relaxed);
    snapshot.server_service_not_found = serviceNotFound_.load(std::memory_order_relaxed);
    snapshot.server_method_not_found = methodNotFound_.load(std::memory_order_relaxed);
    snapshot.server_error_responses = serverErrorResponses_.load(std::memory_order_relaxed);
    snapshot.server_queue_rejected = queueRejected_.load(std::memory_order_relaxed);
    snapshot.server_current_queue_size = currentQueueSize_.load(std::memory_order_relaxed);
    return snapshot;
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
    totalRequests_.fetch_add(1, std::memory_order_relaxed);
    const auto startTime = std::chrono::steady_clock::now();
    const std::uint64_t requestId = request.request_id;
    const RpcTraceContext traceContext = makeTraceContext(request);
    MR_LOG_DEBUG("RPC request received " + traceLogPrefix(traceContext));
    if (!request.serializer.empty() && request.serializer != RawStringSerializer().name()) {
        sendError(connection, request, RpcErrorCode::DeserializationError,
                  "unsupported request serializer: " + request.serializer, startTime);
        return;
    }

    const LookupResult lookup =
        registry_.findMethod(request.service_name, request.method_name);
    if (lookup.status == LookupStatus::ServiceNotFound) {
        sendError(connection, request, RpcErrorCode::ServiceNotFound,
                  "service not found: " + request.service_name, startTime);
        return;
    }
    if (lookup.status == LookupStatus::MethodNotFound) {
        sendError(connection, request, RpcErrorCode::MethodNotFound,
                  "method not found: " + request.service_name + "." +
                      request.method_name,
                  startTime);
        return;
    }

    std::weak_ptr<minireactor::TcpConnection> weakConnection = connection;
    MethodHandler handler = lookup.handler;
    currentQueueSize_.fetch_add(1, std::memory_order_relaxed);
    const SubmitResult result = businessPool_.try_post(
        [this, weakConnection, handler = std::move(handler),
         request = std::move(request), startTime]() mutable {
            RpcResponse response;
            response.request_id = request.request_id;
            response.trace_id = request.trace_id;
            response.serializer = request.serializer.empty() ? "raw" : request.serializer;
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
            response.trace_id = request.trace_id;
            response.serializer = request.serializer.empty() ? "raw" : request.serializer;
            response.server_cost_us = elapsedMicros(startTime);
            if (response.error_code == 0 && request.timeout_ms != 0 &&
                response.server_cost_us / 1000U > request.timeout_ms) {
                response.error_code = static_cast<int>(RpcErrorCode::Timeout);
                response.error_message = "RPC request exceeded server deadline";
                response.payload.clear();
            }
            currentQueueSize_.fetch_sub(1, std::memory_order_relaxed);

            std::string frame;
            try {
                frame = codec_.encodeResponse(response);
            } catch (const std::exception& error) {
                response.error_code = static_cast<int>(RpcErrorCode::ServerError);
                response.error_message =
                    std::string("failed to encode RPC response: ") + error.what();
                response.payload.clear();
                response.server_cost_us = elapsedMicros(startTime);
                frame = codec_.encodeResponse(response);
            }
            recordResponse(response);
            if (std::shared_ptr<minireactor::TcpConnection> activeConnection =
                    weakConnection.lock()) {
                MR_LOG_DEBUG("RPC response sent trace_id=" + response.trace_id +
                             " request_id=" + std::to_string(response.request_id) +
                             " error_code=" + std::to_string(response.error_code));
                activeConnection->send(std::move(frame));
            }
        });

    if (result != SubmitResult::accepted) {
        currentQueueSize_.fetch_sub(1, std::memory_order_relaxed);
        queueRejected_.fetch_add(1, std::memory_order_relaxed);
        sendError(connection, request, RpcErrorCode::ServerError,
                  "server business queue is full", startTime);
    }
    (void)requestId;
}

void RpcServer::sendError(
    const std::shared_ptr<minireactor::TcpConnection>& connection,
    const RpcRequest& request, RpcErrorCode errorCode, std::string errorMessage,
    std::chrono::steady_clock::time_point startTime) {
    RpcResponse response;
    response.request_id = request.request_id;
    response.error_code = static_cast<int>(errorCode);
    response.error_message = std::move(errorMessage);
    response.trace_id = request.trace_id;
    response.serializer = request.serializer.empty() ? "raw" : request.serializer;
    response.server_cost_us = elapsedMicros(startTime);
    recordResponse(response);
    MR_LOG_DEBUG("RPC error response sent trace_id=" + response.trace_id +
                 " request_id=" + std::to_string(response.request_id) +
                 " error_code=" + std::to_string(response.error_code));
    connection->send(codec_.encodeResponse(response));
}

void RpcServer::recordResponse(const RpcResponse& response) {
    if (response.error_code == 0) {
        successResponses_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    failedResponses_.fetch_add(1, std::memory_order_relaxed);
    const auto errorCode = static_cast<RpcErrorCode>(response.error_code);
    switch (errorCode) {
    case RpcErrorCode::Timeout:
        timeoutResponses_.fetch_add(1, std::memory_order_relaxed);
        break;
    case RpcErrorCode::ServiceNotFound:
        serviceNotFound_.fetch_add(1, std::memory_order_relaxed);
        break;
    case RpcErrorCode::MethodNotFound:
        methodNotFound_.fetch_add(1, std::memory_order_relaxed);
        break;
    case RpcErrorCode::ServerError:
        serverErrorResponses_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

std::uint64_t RpcServer::elapsedMicros(
    std::chrono::steady_clock::time_point startTime) {
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    return micros <= 0 ? 1 : static_cast<std::uint64_t>(micros);
}

}  // namespace minirpc
