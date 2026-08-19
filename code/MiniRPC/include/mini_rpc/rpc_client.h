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

    // 自动重试：maxRetries 表示失败后最多再尝试几次（默认 2，0 表示不重试）。
    // 只对 NetworkError / Timeout 重试，总耗时受 timeout 预算约束。
    void setMaxRetries(std::size_t maxRetries);
    // 自动重连退避策略：baseDelay 首次等待，之后指数翻倍直至 maxDelay。
    void setReconnectPolicy(std::chrono::milliseconds baseDelay,
                            std::chrono::milliseconds maxDelay);
    // 开关自动重连（默认开启；connect() 打开、disconnect() 关闭）。
    void setReconnectEnabled(bool enabled);

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
        std::size_t reconnectAttempts{0};   // 已连续重连失败的次数（成功时清零）
        bool reconnectScheduled{false};     // 是否已有挂起的重连定时器
        std::uint64_t reconnectTimerId{0};  // 对应 minireactor::EventLoop::TimerId
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
    // 自动重连：断连后安排指数退避定时器，到期 tryReconnect 重新 connect。
    void scheduleReconnect(const std::shared_ptr<Session>& session);
    // 取消挂起的重连定时器（调用方须持有 sessionsMutex_ 或处于 loop 线程）。
    void cancelReconnect(const std::shared_ptr<Session>& session);
    // loop 线程中执行的实际重连尝试（weak 捕获，Session 可能已释放）。
    void tryReconnect(const std::weak_ptr<Session>& weakSession);
    static double reconnectDelay(double baseDelay, double maxDelay, std::size_t attempts);
    static bool isRetryable(int errorCode);
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
    bool reconnectEnabled_{true};
    double reconnectBaseDelay_{1.0};   // 秒
    double reconnectMaxDelay_{30.0};   // 秒
    std::size_t maxRetries_{2};

    std::mutex pendingMutex_;
    bool acceptingCalls_{false};
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pendingCalls_;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace minirpc
