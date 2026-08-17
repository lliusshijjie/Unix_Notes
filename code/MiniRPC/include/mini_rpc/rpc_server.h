#pragma once

#include "ThreadPool.h"
#include "base/NonCopyable.h"
#include "mini_rpc/codec.h"
#include "mini_rpc/rpc_metrics.h"
#include "mini_rpc/service_registry.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace minireactor {
class Buffer;
class EventLoop;
}

namespace minirpc {

class RpcServer : private minireactor::NonCopyable {
public:
    RpcServer(minireactor::EventLoop* loop, const minireactor::InetAddress& address,
              std::size_t ioThreadCount, std::size_t workerThreadCount,
              std::size_t queueCapacity);

    bool registerMethod(std::string serviceName, std::string methodName,
                        MethodHandler handler);
    void start();
    RpcMetricsSnapshot metrics() const;

private:
    void onMessage(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   minireactor::Buffer* buffer);
    void handleRequest(const std::shared_ptr<minireactor::TcpConnection>& connection,
                       RpcRequest request);
    void sendError(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   const RpcRequest& request, RpcErrorCode errorCode,
                   std::string errorMessage,
                   std::chrono::steady_clock::time_point startTime);
    void recordResponse(const RpcResponse& response);
    static std::uint64_t elapsedMicros(
        std::chrono::steady_clock::time_point startTime);

    minireactor::TcpServer tcpServer_;
    RpcCodec codec_;
    ServiceRegistry registry_;
    ThreadPool businessPool_;

    std::atomic<std::uint64_t> totalRequests_{0};
    std::atomic<std::uint64_t> successResponses_{0};
    std::atomic<std::uint64_t> failedResponses_{0};
    std::atomic<std::uint64_t> timeoutResponses_{0};
    std::atomic<std::uint64_t> serviceNotFound_{0};
    std::atomic<std::uint64_t> methodNotFound_{0};
    std::atomic<std::uint64_t> serverErrorResponses_{0};
    std::atomic<std::uint64_t> queueRejected_{0};
    std::atomic<std::uint64_t> currentQueueSize_{0};
};

}  // namespace minirpc
