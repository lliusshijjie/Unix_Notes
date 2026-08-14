#pragma once

#include "ThreadPool.h"
#include "base/NonCopyable.h"
#include "mini_rpc/codec.h"
#include "mini_rpc/service_registry.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

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

private:
    void onMessage(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   minireactor::Buffer* buffer);
    void handleRequest(const std::shared_ptr<minireactor::TcpConnection>& connection,
                       RpcRequest request);
    void sendError(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   std::uint64_t requestId, RpcErrorCode errorCode,
                   std::string errorMessage);

    minireactor::TcpServer tcpServer_;
    RpcCodec codec_;
    ServiceRegistry registry_;
    ThreadPool businessPool_;
};

}  // namespace minirpc
