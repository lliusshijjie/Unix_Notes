#pragma once

#include "base/NonCopyable.h"
#include "mini_rpc/codec.h"
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

namespace minireactor {
class Buffer;
class EventLoop;
class TcpClient;
class TcpConnection;
}

namespace minirpc {

class RpcClient : private minireactor::NonCopyable {
public:
    RpcClient(std::string serverIp, std::uint16_t serverPort);
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

    void onConnection(const std::shared_ptr<minireactor::TcpConnection>& connection);
    void onConnectionError(int error);
    void onMessage(const std::shared_ptr<minireactor::TcpConnection>& connection,
                   minireactor::Buffer* buffer);
    void onTimeout(std::uint64_t requestId);
    std::shared_ptr<PendingCall> takePending(std::uint64_t requestId);
    void completeResponse(RpcResponse response);
    void completeCall(const std::shared_ptr<PendingCall>& pending, RpcResponse response);
    void failAllPending(RpcErrorCode errorCode, const std::string& errorMessage);
    std::uint64_t nextRequestId();
    static RpcResponse errorResponse(std::uint64_t requestId, RpcErrorCode errorCode,
                                     std::string errorMessage);

    minireactor::EventLoopThread loopThread_;
    minireactor::EventLoop* loop_{nullptr};
    std::unique_ptr<minireactor::TcpClient> tcpClient_;
    RpcCodec codec_;
    std::atomic<std::uint64_t> nextRequestId_{1};

    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    bool connecting_{false};
    bool connected_{false};
    bool acceptingConnection_{false};
    bool stopping_{false};
    int connectionError_{0};

    std::mutex pendingMutex_;
    bool acceptingCalls_{false};
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pendingCalls_;
};

}  // namespace minirpc
