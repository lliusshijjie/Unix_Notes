#include "mini_rpc/rpc_client.h"

#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"

#include <algorithm>
#include <future>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace minirpc {

namespace {

constexpr std::chrono::milliseconds kInitialReconnectDelay{100};
constexpr std::chrono::milliseconds kMaxReconnectDelay{3000};

}  // namespace

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
    alive_->store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
        desiredConnected_ = false;
        connectionState_ = ClientConnectionState::Stopping;
    }
    stateCondition_.notify_all();
    cancelReconnectTimer();
    failAllPending(RpcErrorCode::ClientStopping, "RPC client is stopping");

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

void RpcClient::setConnectionStateCallback(ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    connectionStateCallback_ = std::move(callback);
}

bool RpcClient::connect(std::chrono::milliseconds timeout) {
    if (loop_->isInLoopThread()) {
        throw std::logic_error("RpcClient::connect cannot block its EventLoop thread");
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return false;
    }

    bool startConnection = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (stopping_) {
            return false;
        }
        desiredConnected_ = true;
        connectionError_ = 0;
        if (connectionState_ == ClientConnectionState::Connected) {
            return true;
        }
        if (connectionState_ == ClientConnectionState::Disconnected) {
            connectionState_ = ClientConnectionState::Connecting;
            startConnection = true;
        }
    }
    if (startConnection) {
        startConnectIfNeeded(ClientConnectionState::Connecting);
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool observedTerminalState =
        stateCondition_.wait_for(lock, timeout, [this] {
            return connectionState_ == ClientConnectionState::Connected ||
                connectionState_ == ClientConnectionState::Disconnected ||
                stopping_;
        });
    if (!observedTerminalState ||
        connectionState_ != ClientConnectionState::Connected) {
        desiredConnected_ = false;
        connectionState_ = ClientConnectionState::Disconnected;
        lock.unlock();
        cancelReconnectTimer();
        tcpClient_->stop();
        stateCondition_.notify_all();
        return false;
    }
    return true;
}

void RpcClient::disconnect() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        desiredConnected_ = false;
        connectionState_ = ClientConnectionState::Disconnected;
    }
    stateCondition_.notify_all();
    cancelReconnectTimer();
    failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
    tcpClient_->stop();
    if (!loop_->isInLoopThread()) {
        std::promise<void> stopped;
        std::future<void> stoppedFuture = stopped.get_future();
        loop_->queueInLoop([&stopped] { stopped.set_value(); });
        stoppedFuture.get();
    }
    updateConnectionState(ClientConnectionState::Disconnected);
}

bool RpcClient::connected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connectionState_ == ClientConnectionState::Connected;
}

RpcMetricsSnapshot RpcClient::metrics() const {
    RpcMetricsSnapshot snapshot;
    snapshot.client_total_calls = totalCalls_.load(std::memory_order_relaxed);
    snapshot.client_success_calls = successCalls_.load(std::memory_order_relaxed);
    snapshot.client_failed_calls = failedCalls_.load(std::memory_order_relaxed);
    snapshot.client_timeout_calls = timeoutCalls_.load(std::memory_order_relaxed);
    snapshot.client_cancelled_calls = cancelledCalls_.load(std::memory_order_relaxed);
    snapshot.client_reconnect_count = reconnectCount_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        snapshot.client_pending_calls = pendingCalls_.size();
    }
    return snapshot;
}

RpcFuture RpcClient::callAsync(RpcRequest request, RpcCallOptions options) {
    return callAsyncInternal(std::move(request), std::move(options), {});
}

void RpcClient::callAsync(RpcRequest request, RpcCallback callback, RpcCallOptions options) {
    (void)callAsyncInternal(std::move(request), std::move(options), std::move(callback));
}

RpcFuture RpcClient::callAsyncInternal(RpcRequest request, RpcCallOptions options,
                                       RpcCallback callback) {
    const std::uint64_t requestId = nextRequestId();
    totalCalls_.fetch_add(1, std::memory_order_relaxed);
    request.request_id = requestId;
    request.timeout_ms = static_cast<std::uint64_t>(options.timeout.count());
    request.trace_id =
        options.trace_id.empty() ? "rpc-" + std::to_string(requestId) : options.trace_id;
    if (request.serializer.empty()) {
        request.serializer = "raw";
    }
    if (request.attempt == 0) {
        request.attempt = 1;
    }

    auto futureState = std::make_shared<RpcFuture::State>();
    RpcFuture future(futureState);

    if (options.timeout <= std::chrono::milliseconds::zero()) {
        RpcResponse response = errorResponse(requestId, RpcErrorCode::Timeout,
                                             "RPC call timed out before it was sent");
        recordClientResponse(response);
        RpcFuture::complete(futureState, response);
        if (callback) {
            callback(std::move(response));
        }
        return future;
    }

    std::string frame;
    try {
        frame = codec_.encodeRequest(request);
    } catch (const std::exception& error) {
        RpcResponse response =
            errorResponse(requestId, RpcErrorCode::ProtocolError, error.what());
        recordClientResponse(response);
        RpcFuture::complete(futureState, response);
        if (callback) {
            callback(std::move(response));
        }
        return future;
    }

    auto pending = std::make_shared<PendingCall>();
    pending->requestId = requestId;
    pending->request = std::move(request);
    pending->options = std::move(options);
    pending->deadline = std::chrono::steady_clock::now() + pending->options.timeout;
    pending->callback = std::move(callback);
    pending->futureState = futureState;
    pending->frame = std::move(frame);

    std::shared_ptr<std::atomic<bool>> alive = alive_;
    RpcFuture::setCancel(futureState, [this, alive, requestId] {
        if (alive->load(std::memory_order_acquire)) {
            cancelPending(requestId);
        }
    });

    pending->timerId = loop_->runAfter(
        toSeconds(pending->options.timeout), [this, requestId] { onDeadline(requestId); });

    bool shouldStartConnect = false;
    bool shouldFailFast = false;
    bool clientStopping = false;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        clientStopping = stopping_;
        if (clientStopping) {
            shouldFailFast = true;
        } else if (connectionState_ == ClientConnectionState::Connected) {
        } else if (pending->options.fail_fast) {
            shouldFailFast = true;
        } else {
            desiredConnected_ = true;
            shouldStartConnect =
                connectionState_ == ClientConnectionState::Disconnected;
            if (shouldStartConnect) {
                connectionState_ = ClientConnectionState::Connecting;
            }
        }
    }

    if (shouldFailFast) {
        loop_->cancel(pending->timerId);
        RpcResponse response = errorResponse(
            requestId,
            clientStopping ? RpcErrorCode::ClientStopping : RpcErrorCode::NetworkError,
            clientStopping ? "RPC client is stopping" : "RPC client is not connected");
        recordClientResponse(response);
        RpcFuture::complete(futureState, response);
        if (callback) {
            callback(std::move(response));
        }
        return future;
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCalls_.emplace(requestId, pending);
    }

    if (shouldStartConnect) {
        startConnectIfNeeded(ClientConnectionState::Connecting);
    }
    sendPendingCalls();
    return future;
}

RpcResponse RpcClient::call(RpcRequest request, std::chrono::milliseconds timeout) {
    if (loop_->isInLoopThread()) {
        throw std::logic_error("RpcClient::call cannot block its EventLoop thread");
    }
    RpcCallOptions options;
    options.timeout = timeout;
    return callAsync(std::move(request), options).get();
}

void RpcClient::onConnection(
    const std::shared_ptr<minireactor::TcpConnection>& connection) {
    if (connection->state() == minireactor::TcpConnection::State::kConnected) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            reconnectDelay_ = kInitialReconnectDelay;
        }
        updateConnectionState(ClientConnectionState::Connected);
        sendPendingCalls();
        return;
    }

    bool shouldReconnect = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        shouldReconnect = desiredConnected_ && !stopping_;
    }
    failSentPending(RpcErrorCode::NetworkError, "RPC connection closed");
    if (shouldReconnect) {
        updateConnectionState(ClientConnectionState::Reconnecting);
        scheduleReconnect();
    } else {
        updateConnectionState(ClientConnectionState::Disconnected);
        failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
    }
}

void RpcClient::onConnectionError(int error) {
    const std::string message =
        std::error_code(error, std::generic_category()).message();

    bool shouldReconnect = false;
    std::vector<std::shared_ptr<PendingCall>> exhausted;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        connectionError_ = error;
        shouldReconnect = desiredConnected_ && !stopping_;
    }
    if (shouldReconnect) {
        shouldReconnect = expireRetryExhaustedPending(exhausted);
    }
    for (auto& item : exhausted) {
        completePending(item, errorResponse(item->requestId, RpcErrorCode::RetryExhausted,
                                            "RPC retry attempts exhausted"));
    }

    if (shouldReconnect) {
        updateConnectionState(ClientConnectionState::Reconnecting);
        scheduleReconnect();
        return;
    }

    updateConnectionState(ClientConnectionState::Disconnected);
    failAllPending(RpcErrorCode::NetworkError, message);
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
            failAllPending(RpcErrorCode::ProtocolError,
                           "server returned an invalid RPC response");
            tcpClient_->stop();
            return;
        }
        completeResponse(std::move(response));
    }
}

void RpcClient::onDeadline(std::uint64_t requestId) {
    failPending(requestId, RpcErrorCode::Timeout, "RPC call timed out");
}

void RpcClient::cancelPending(std::uint64_t requestId) {
    failPending(requestId, RpcErrorCode::Cancelled, "RPC call cancelled");
}

void RpcClient::sendPendingCalls() {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (connectionState_ != ClientConnectionState::Connected) {
            return;
        }
    }

    std::vector<std::shared_ptr<PendingCall>> ready;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        ready.reserve(pendingCalls_.size());
        for (auto& item : pendingCalls_) {
            if (item.second->state == PendingState::WaitingToSend) {
                ready.push_back(item.second);
            }
        }
    }
    for (const auto& pending : ready) {
        sendPendingCall(pending);
    }
}

bool RpcClient::sendPendingCall(const std::shared_ptr<PendingCall>& pending) {
    if (std::chrono::steady_clock::now() >= pending->deadline) {
        failPending(pending->requestId, RpcErrorCode::QueueingTimeout,
                    "RPC call timed out before it was sent");
        return false;
    }

    std::shared_ptr<minireactor::TcpConnection> connection = tcpClient_->connection();
    if (!connection) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const auto found = pendingCalls_.find(pending->requestId);
        if (found == pendingCalls_.end() || found->second != pending ||
            pending->state != PendingState::WaitingToSend) {
            return false;
        }
        pending->state = PendingState::Sent;
    }
    connection->send(pending->frame);
    return true;
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
        pending->state = PendingState::Completed;
    }
    completePending(std::move(pending), std::move(response));
}

void RpcClient::completePending(std::shared_ptr<PendingCall> pending,
                                RpcResponse response) {
    if (pending->timerId != 0) {
        loop_->cancel(pending->timerId);
        pending->timerId = 0;
    }
    recordClientResponse(response);
    RpcFuture::complete(pending->futureState, std::move(response));
    if (pending->callback) {
        pending->callback(RpcFuture(pending->futureState).get());
    }
}

void RpcClient::failPending(std::uint64_t requestId, RpcErrorCode errorCode,
                            std::string errorMessage) {
    std::shared_ptr<PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const auto found = pendingCalls_.find(requestId);
        if (found == pendingCalls_.end()) {
            return;
        }
        pending = found->second;
        pendingCalls_.erase(found);
        if (errorCode == RpcErrorCode::Timeout ||
            errorCode == RpcErrorCode::QueueingTimeout) {
            pending->state = PendingState::TimedOut;
        } else if (errorCode == RpcErrorCode::Cancelled) {
            pending->state = PendingState::Cancelled;
        } else {
            pending->state = PendingState::Failed;
        }
    }
    completePending(std::move(pending),
                    errorResponse(requestId, errorCode, std::move(errorMessage)));
}

void RpcClient::failAllPending(RpcErrorCode errorCode,
                               const std::string& errorMessage) {
    std::vector<std::shared_ptr<PendingCall>> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.reserve(pendingCalls_.size());
        for (auto& item : pendingCalls_) {
            item.second->state = PendingState::Failed;
            pending.push_back(std::move(item.second));
        }
        pendingCalls_.clear();
    }
    for (auto& item : pending) {
        completePending(item, errorResponse(item->requestId, errorCode, errorMessage));
    }
}

void RpcClient::failSentPending(RpcErrorCode errorCode,
                                const std::string& errorMessage) {
    std::vector<std::shared_ptr<PendingCall>> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto iter = pendingCalls_.begin(); iter != pendingCalls_.end();) {
            if (iter->second->state == PendingState::Sent || iter->second->options.fail_fast) {
                iter->second->state = PendingState::Failed;
                pending.push_back(std::move(iter->second));
                iter = pendingCalls_.erase(iter);
            } else {
                ++iter;
            }
        }
    }
    for (auto& item : pending) {
        completePending(item, errorResponse(item->requestId, errorCode, errorMessage));
    }
}

bool RpcClient::expireRetryExhaustedPending(
    std::vector<std::shared_ptr<PendingCall>>& exhausted) {
    bool hasWaiting = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto iter = pendingCalls_.begin(); iter != pendingCalls_.end();) {
            std::shared_ptr<PendingCall> pending = iter->second;
            if (pending->state != PendingState::WaitingToSend) {
                ++iter;
                continue;
            }

            if (pending->options.retry_enabled) {
                const std::uint32_t completedRetries =
                    pending->request.attempt == 0 ? 0 : pending->request.attempt - 1;
                if (completedRetries >= pending->options.max_retries) {
                    pending->state = PendingState::Failed;
                    exhausted.push_back(std::move(pending));
                    iter = pendingCalls_.erase(iter);
                    continue;
                }
                ++pending->request.attempt;
            }
            hasWaiting = true;
            ++iter;
        }
    }
    return hasWaiting;
}

void RpcClient::recordClientResponse(const RpcResponse& response) {
    if (response.error_code == 0) {
        successCalls_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    failedCalls_.fetch_add(1, std::memory_order_relaxed);
    const auto errorCode = static_cast<RpcErrorCode>(response.error_code);
    if (errorCode == RpcErrorCode::Timeout ||
        errorCode == RpcErrorCode::QueueingTimeout) {
        timeoutCalls_.fetch_add(1, std::memory_order_relaxed);
    } else if (errorCode == RpcErrorCode::Cancelled) {
        cancelledCalls_.fetch_add(1, std::memory_order_relaxed);
    }
}

void RpcClient::startConnectIfNeeded(ClientConnectionState nextState) {
    updateConnectionState(nextState);
    tcpClient_->connect();
}

void RpcClient::scheduleReconnect() {
    std::chrono::milliseconds delay;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!desiredConnected_ || stopping_ || reconnectTimerId_ != 0) {
            return;
        }
        delay = reconnectDelay_;
        reconnectDelay_ = std::min(
            kMaxReconnectDelay, std::chrono::milliseconds(reconnectDelay_.count() * 2));
    }
    const std::uint64_t timerId = loop_->runAfter(toSeconds(delay), [this] {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            reconnectTimerId_ = 0;
            if (!desiredConnected_ || stopping_) {
                return;
            }
        }
        startConnectIfNeeded(ClientConnectionState::Reconnecting);
    });
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        reconnectTimerId_ = timerId;
    }
}

void RpcClient::cancelReconnectTimer() {
    std::uint64_t timerId = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        timerId = reconnectTimerId_;
        reconnectTimerId_ = 0;
    }
    if (timerId != 0) {
        loop_->cancel(timerId);
    }
}

void RpcClient::updateConnectionState(ClientConnectionState state) {
    ConnectionStateCallback callback;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connectionState_ = state;
        callback = connectionStateCallback_;
    }
    stateCondition_.notify_all();
    if (callback) {
        callback(state);
    }
    if (state == ClientConnectionState::Reconnecting) {
        reconnectCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RpcClient::hasWaitingPendingLocked() const {
    for (const auto& item : pendingCalls_) {
        if (item.second->state == PendingState::WaitingToSend) {
            return true;
        }
    }
    return false;
}

double RpcClient::toSeconds(std::chrono::milliseconds duration) {
    return static_cast<double>(duration.count()) / 1000.0;
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
