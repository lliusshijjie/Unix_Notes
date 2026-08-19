#pragma once

#include "base/NonCopyable.h"
#include "mini_rpc/codec.h"
#include "mini_rpc/endpoint.h"
#include "mini_rpc/load_balancer.h"
#include "mini_rpc/service_discovery.h"
#include "net/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace minireactor {
class Buffer;
class EventLoop;
class InetAddress;
class TcpClient;
class TcpConnection;
}

namespace minirpc {

class RpcClient : private minireactor::NonCopyable {
public:
    RpcClient(std::string serverIp, std::uint16_t serverPort);
    RpcClient(std::string serviceName, std::shared_ptr<ServiceDiscovery> discovery);
    ~RpcClient();

    bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    void disconnect();
    bool connected() const;
    std::future<RpcResponse> asyncCall(
        RpcRequest request,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    RpcResponse call(RpcRequest request,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

private:
    struct PendingCall {
        std::promise<RpcResponse> promise;
        std::atomic<bool> completed{false};
        std::uint64_t timerId{0};
    };

    struct Session {
        Endpoint endpoint;
        std::unique_ptr<minireactor::TcpClient> tcpClient;
        bool connected{false};
        bool connecting{false};
    };

    void createSession(const Endpoint& endpoint);
    std::shared_ptr<Session> findSession(const Endpoint& endpoint) const;
    bool hasConnectedSession() const;
    std::vector<Endpoint> readyEndpoints(const std::vector<Endpoint>& endpoints) const;
    std::shared_ptr<minireactor::TcpConnection> selectConnection(const RpcRequest& request,
                                                                 RpcErrorCode& errorCode,
                                                                 std::string& errorMessage);
    void onConnection(const std::string& key,
                      const std::shared_ptr<minireactor::TcpConnection>& connection);
    void onConnectionError(const std::string& key, int error);
    void onMessage(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   minireactor::Buffer* buffer);
    void onTimeout(std::uint64_t requestId);
    std::shared_ptr<PendingCall> takePending(std::uint64_t requestId);
    void completeResponse(RpcResponse response);
    void completeCall(const std::shared_ptr<PendingCall>& pending, RpcResponse response);
    void failAllPending(RpcErrorCode errorCode, const std::string& errorMessage);
    void stopSession(const std::shared_ptr<Session>& session);
    std::uint64_t nextRequestId();
    static RpcResponse errorResponse(std::uint64_t requestId, RpcErrorCode errorCode,
                                     std::string errorMessage);

    minireactor::EventLoopThread loopThread_;
    minireactor::EventLoop* loop_{nullptr};
    std::shared_ptr<ServiceDiscovery> discovery_;
    std::shared_ptr<LoadBalancer> loadBalancer_;
    std::string serviceName_;
    RpcCodec codec_;
    std::atomic<std::uint64_t> nextRequestId_{1};

    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    int connectingCount_{0};
    bool connected_{false};
    bool acceptingConnection_{false};
    bool stopping_{false};

    std::mutex pendingMutex_;
    bool acceptingCalls_{false};
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pendingCalls_;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace minirpc
