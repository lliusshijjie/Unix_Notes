#pragma once

#include "base/NonCopyable.h"
#include "mini_rpc/codec.h"
#include "mini_rpc/rpc_call_options.h"
#include "mini_rpc/rpc_future.h"
#include "mini_rpc/rpc_metrics.h"
#include "net/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace minireactor {
class Buffer;
class EventLoop;
class TcpClient;
class TcpConnection;
}

namespace minirpc {

using RpcCallback = std::function<void(RpcResponse)>;

enum class ClientConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Stopping
};

class RpcClient : private minireactor::NonCopyable {
public:
    using ConnectionStateCallback = std::function<void(ClientConnectionState)>;

    RpcClient(std::string serverIp, std::uint16_t serverPort);
    ~RpcClient();

    void setConnectionStateCallback(ConnectionStateCallback callback);

    bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    void disconnect();
    bool connected() const;

    RpcFuture callAsync(RpcRequest request, RpcCallOptions options = {});
    void callAsync(RpcRequest request, RpcCallback callback, RpcCallOptions options = {});

    RpcResponse call(RpcRequest request,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    RpcMetricsSnapshot metrics() const;

private:
    enum class PendingState {
        WaitingToSend,
        Sent,
        Completed,
        TimedOut,
        Cancelled,
        Failed
    };

    struct PendingCall {
        std::uint64_t requestId{0};
        RpcRequest request;
        RpcCallOptions options;
        PendingState state{PendingState::WaitingToSend};
        std::chrono::steady_clock::time_point deadline;
        RpcCallback callback;
        std::shared_ptr<RpcFuture::State> futureState;
        std::uint64_t timerId{0};
        std::string frame;
    };

    void onConnection(const std::shared_ptr<minireactor::TcpConnection>& connection);
    void onConnectionError(int error);
    void onMessage(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   minireactor::Buffer* buffer);
    void onDeadline(std::uint64_t requestId);
    void cancelPending(std::uint64_t requestId);
    RpcFuture callAsyncInternal(RpcRequest request, RpcCallOptions options,
                                RpcCallback callback);
    void sendPendingCalls();
    bool sendPendingCall(const std::shared_ptr<PendingCall>& pending);
    void completeResponse(RpcResponse response);
    void completePending(std::shared_ptr<PendingCall> pending, RpcResponse response);
    void failPending(std::uint64_t requestId, RpcErrorCode errorCode,
                     std::string errorMessage);
    void failAllPending(RpcErrorCode errorCode, const std::string& errorMessage);
    void failSentPending(RpcErrorCode errorCode, const std::string& errorMessage);
    bool expireRetryExhaustedPending(std::vector<std::shared_ptr<PendingCall>>& exhausted);
    void recordClientResponse(const RpcResponse& response);
    void startConnectIfNeeded(ClientConnectionState nextState);
    void scheduleReconnect();
    void cancelReconnectTimer();
    void updateConnectionState(ClientConnectionState state);
    bool hasWaitingPendingLocked() const;
    static double toSeconds(std::chrono::milliseconds duration);
    std::uint64_t nextRequestId();
    static RpcResponse errorResponse(std::uint64_t requestId, RpcErrorCode errorCode,
                                     std::string errorMessage);

    minireactor::EventLoopThread loopThread_;
    minireactor::EventLoop* loop_{nullptr};
    std::unique_ptr<minireactor::TcpClient> tcpClient_;
    RpcCodec codec_;
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};

    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    ClientConnectionState connectionState_{ClientConnectionState::Disconnected};
    bool desiredConnected_{false};
    bool stopping_{false};
    int connectionError_{0};
    std::chrono::milliseconds reconnectDelay_{100};
    std::uint64_t reconnectTimerId_{0};
    ConnectionStateCallback connectionStateCallback_;

    std::atomic<std::uint64_t> totalCalls_{0};
    std::atomic<std::uint64_t> successCalls_{0};
    std::atomic<std::uint64_t> failedCalls_{0};
    std::atomic<std::uint64_t> timeoutCalls_{0};
    std::atomic<std::uint64_t> cancelledCalls_{0};
    std::atomic<std::uint64_t> reconnectCount_{0};

    mutable std::mutex pendingMutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pendingCalls_;
};

}  // namespace minirpc
