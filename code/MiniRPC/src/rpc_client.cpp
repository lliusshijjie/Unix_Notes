#include "mini_rpc/rpc_client.h"

#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"

#include <future>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace minirpc {

RpcClient::RpcClient(std::string serverIp, std::uint16_t serverPort) {
    minireactor::InetAddress address(std::move(serverIp), serverPort);
    loop_ = loopThread_.startLoop();

    std::promise<void> created;
    std::future<void> createdFuture = created.get_future();
    loop_->queueInLoop([this, address = std::move(address), &created] {
        try {
            tcpClient_ =
                std::make_unique<minireactor::TcpClient>(loop_, address, "mini-rpc-client");
            tcpClient_->setConnectionCallback(
                [this](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                    onConnection(connection);
                });
            tcpClient_->setConnectionErrorCallback(
                [this](int error) { onConnectionError(error); });
            tcpClient_->setMessageCallback(
                [this](const std::shared_ptr<minireactor::TcpConnection>& connection,
                       minireactor::Buffer* buffer) { onMessage(connection, buffer); });
            created.set_value();
        } catch (...) {
            created.set_exception(std::current_exception());
        }
    });
    createdFuture.get();
}

RpcClient::~RpcClient() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
        acceptingConnection_ = false;
        connecting_ = false;
        connected_ = false;
    }
    stateCondition_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = false;
    }
    failAllPending(RpcErrorCode::NetworkError, "RPC client is stopping");

    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    loop_->queueInLoop([this, &destroyed] {
        try {
            if (tcpClient_) {
                tcpClient_->stop();
                tcpClient_.reset();
            }
            destroyed.set_value();
        } catch (...) {
            destroyed.set_exception(std::current_exception());
        }
    });
    destroyedFuture.get();
    loop_->quit();
}

bool RpcClient::connect(std::chrono::milliseconds timeout) {
    if (loop_->isInLoopThread()) {
        throw std::logic_error("RpcClient::connect cannot block its EventLoop thread");
    }

    bool startConnection = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (stopping_) {
            return false;
        }
        if (connected_) {
            return true;
        }
        if (!connecting_) {
            connecting_ = true;
            acceptingConnection_ = true;
            connectionError_ = 0;
            startConnection = true;
        }
    }
    if (startConnection) {
        tcpClient_->connect();
    }

    bool timedOut = false;
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (!stateCondition_.wait_for(lock, timeout, [this] { return !connecting_; })) {
        connecting_ = false;
        acceptingConnection_ = false;
        timedOut = true;
    }
    const bool result = connected_;
    lock.unlock();
    if (timedOut) {
        tcpClient_->stop();
    }
    return result;
}

void RpcClient::disconnect() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        acceptingConnection_ = false;
        connecting_ = false;
        connected_ = false;
    }
    stateCondition_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = false;
    }
    failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
    tcpClient_->stop();
}

bool RpcClient::connected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connected_;
}

RpcResponse RpcClient::call(RpcRequest request, std::chrono::milliseconds timeout) {
    if (loop_->isInLoopThread()) {
        throw std::logic_error("RpcClient::call cannot block its EventLoop thread");
    }

    const std::uint64_t requestId = nextRequestId();
    request.request_id = requestId;
    if (!connected()) {
        return errorResponse(requestId, RpcErrorCode::NetworkError,
                             "RPC client is not connected");
    }

    std::string frame;
    try {
        frame = codec_.encodeRequest(request);
    } catch (const std::exception& error) {
        return errorResponse(requestId, RpcErrorCode::ProtocolError, error.what());
    }

    auto pending = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (!acceptingCalls_) {
            return errorResponse(requestId, RpcErrorCode::NetworkError,
                                 "RPC connection is closing");
        }
        pendingCalls_.emplace(requestId, pending);
    }

    std::shared_ptr<minireactor::TcpConnection> connection = tcpClient_->connection();
    if (!connection) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCalls_.erase(requestId);
        return errorResponse(requestId, RpcErrorCode::NetworkError,
                             "RPC connection is unavailable");
    }
    connection->send(std::move(frame));

    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (pending->condition.wait_for(lock, timeout,
                                        [&pending] { return pending->completed; })) {
            return pending->response;
        }
    }

    bool ownsTimeout = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const auto found = pendingCalls_.find(requestId);
        if (found != pendingCalls_.end() && found->second == pending) {
            pendingCalls_.erase(found);
            ownsTimeout = true;
        }
    }
    if (ownsTimeout) {
        return errorResponse(requestId, RpcErrorCode::Timeout, "RPC call timed out");
    }

    std::unique_lock<std::mutex> lock(pending->mutex);
    pending->condition.wait(lock, [&pending] { return pending->completed; });
    return pending->response;
}

void RpcClient::onConnection(
    const std::shared_ptr<minireactor::TcpConnection>& connection) {
    if (connection->state() == minireactor::TcpConnection::State::kConnected) {
        bool rejectConnection = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            rejectConnection = stopping_ || !acceptingConnection_;
            connecting_ = false;
            connected_ = !rejectConnection;
        }
        if (!rejectConnection) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            acceptingCalls_ = true;
        }
        stateCondition_.notify_all();
        if (rejectConnection) {
            loop_->queueInLoop([this] {
                if (tcpClient_) {
                    tcpClient_->stop();
                }
            });
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connecting_ = false;
        connected_ = false;
        acceptingConnection_ = false;
    }
    stateCondition_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = false;
    }
    failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
}

void RpcClient::onConnectionError(int error) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connecting_ = false;
        connected_ = false;
        acceptingConnection_ = false;
        connectionError_ = error;
    }
    stateCondition_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = false;
    }
    failAllPending(RpcErrorCode::NetworkError,
                   std::error_code(error, std::generic_category()).message());
}

void RpcClient::onMessage(
    const std::shared_ptr<minireactor::TcpConnection>&,
    minireactor::Buffer* buffer) {
    for (;;) {
        RpcResponse response;
        const DecodeStatus status = codec_.tryDecodeResponse(*buffer, response);
        if (status == DecodeStatus::NeedMoreData) {
            return;
        }
        if (status == DecodeStatus::ProtocolError) {
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                acceptingCalls_ = false;
            }
            failAllPending(RpcErrorCode::ProtocolError,
                           "server returned an invalid RPC response");
            tcpClient_->stop();
            return;
        }
        completeResponse(std::move(response));
    }
}

void RpcClient::completeResponse(RpcResponse response) {
    std::shared_ptr<PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const auto found = pendingCalls_.find(response.request_id);
        if (found == pendingCalls_.end()) {
            return;
        }
        pending = found->second;
        pendingCalls_.erase(found);
    }
    completeCall(pending, std::move(response));
}

void RpcClient::completeCall(const std::shared_ptr<PendingCall>& pending,
                             RpcResponse response) {
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (pending->completed) {
            return;
        }
        pending->response = std::move(response);
        pending->completed = true;
    }
    pending->condition.notify_one();
}

void RpcClient::failAllPending(RpcErrorCode errorCode,
                               const std::string& errorMessage) {
    std::vector<std::pair<std::uint64_t, std::shared_ptr<PendingCall>>> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.reserve(pendingCalls_.size());
        for (auto& item : pendingCalls_) {
            pending.emplace_back(item.first, std::move(item.second));
        }
        pendingCalls_.clear();
    }
    for (auto& item : pending) {
        completeCall(item.second, errorResponse(item.first, errorCode, errorMessage));
    }
}

std::uint64_t RpcClient::nextRequestId() {
    std::uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    if (requestId == 0) {
        requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    }
    return requestId;
}

RpcResponse RpcClient::errorResponse(std::uint64_t requestId,
                                     RpcErrorCode errorCode,
                                     std::string errorMessage) {
    RpcResponse response;
    response.request_id = requestId;
    response.error_code = static_cast<int>(errorCode);
    response.error_message = std::move(errorMessage);
    return response;
}

}  // namespace minirpc
