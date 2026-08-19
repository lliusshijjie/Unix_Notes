#include "mini_rpc/rpc_client.h"

#include "mini_rpc/round_robin_load_balancer.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace minirpc {

RpcClient::RpcClient(std::string serverIp, std::uint16_t serverPort) {
    loop_ = loopThread_.startLoop();
    createSession(Endpoint{std::move(serverIp), serverPort});
}

RpcClient::RpcClient(std::string serviceName, std::shared_ptr<ServiceDiscovery> discovery)
    : discovery_(std::move(discovery)),
      loadBalancer_(std::make_shared<RoundRobinLoadBalancer>()),
      serviceName_(std::move(serviceName)) {
    if (serviceName_.empty() || !discovery_) {
        throw std::invalid_argument("RpcClient requires a service name and ServiceDiscovery");
    }
    loop_ = loopThread_.startLoop();
}

void RpcClient::createSession(const Endpoint& endpoint) {
    const std::string key = toString(endpoint);
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.find(key) != sessions_.end()) {
            return;
        }
    }

    std::promise<void> created;
    std::future<void> createdFuture = created.get_future();
    loop_->queueInLoop([this, endpoint, key, &created] {
        try {
            auto session = std::make_shared<Session>();
            session->endpoint = endpoint;
            session->tcpClient = std::make_unique<minireactor::TcpClient>(
                loop_, minireactor::InetAddress(endpoint.host, endpoint.port),
                "mini-rpc-client-" + key);
            session->tcpClient->setConnectionCallback(
                [this, key](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                    onConnection(key, connection);
                });
            session->tcpClient->setConnectionErrorCallback(
                [this, key](int error) { onConnectionError(key, error); });
            session->tcpClient->setMessageCallback(
                [this](const std::shared_ptr<minireactor::TcpConnection>& connection,
                       minireactor::Buffer* buffer) { onMessage(connection, buffer); });
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                sessions_.emplace(key, std::move(session));
            }
            created.set_value();
        } catch (...) {
            created.set_exception(std::current_exception());
        }
    });
    createdFuture.get();
}

std::shared_ptr<RpcClient::Session> RpcClient::findSession(const Endpoint& endpoint) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    const auto found = sessions_.find(toString(endpoint));
    if (found == sessions_.end()) {
        return nullptr;
    }
    return found->second;
}

bool RpcClient::hasConnectedSession() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const auto& item : sessions_) {
        if (item.second->connected) {
            return true;
        }
    }
    return false;
}

std::vector<Endpoint> RpcClient::readyEndpoints(const std::vector<Endpoint>& endpoints) const {
    std::vector<Endpoint> ready;
    ready.reserve(endpoints.size());
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const Endpoint& endpoint : endpoints) {
        const auto found = sessions_.find(toString(endpoint));
        if (found != sessions_.end() && found->second->connected) {
            ready.push_back(endpoint);
        }
    }
    return ready;
}

std::shared_ptr<minireactor::TcpConnection> RpcClient::selectConnection(
    const RpcRequest& request, RpcErrorCode& errorCode, std::string& errorMessage) {
    if (discovery_) {
        const std::string& serviceName =
            request.service_name.empty() ? serviceName_ : request.service_name;
        const std::vector<Endpoint> endpoints = discovery_->discover(serviceName);
        if (endpoints.empty()) {
            errorCode = RpcErrorCode::ServiceUnavailable;
            errorMessage = "no available endpoint";
            return nullptr;
        }
        const std::vector<Endpoint> ready = readyEndpoints(endpoints);
        if (ready.empty()) {
            errorCode = RpcErrorCode::ServiceUnavailable;
            errorMessage = "no available endpoint";
            return nullptr;
        }
        const Endpoint selected = loadBalancer_->select(ready);
        const std::shared_ptr<Session> session = findSession(selected);
        if (!session || !session->tcpClient) {
            errorCode = RpcErrorCode::NetworkError;
            errorMessage = "RPC connection is unavailable";
            return nullptr;
        }
        std::shared_ptr<minireactor::TcpConnection> connection =
            session->tcpClient->connection();
        if (!connection) {
            errorCode = RpcErrorCode::NetworkError;
            errorMessage = "RPC connection is unavailable";
            return nullptr;
        }
        return connection;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.empty()) {
            errorCode = RpcErrorCode::NetworkError;
            errorMessage = "RPC client is not connected";
            return nullptr;
        }
        session = sessions_.begin()->second;
    }
    if (!session || !session->tcpClient) {
        errorCode = RpcErrorCode::NetworkError;
        errorMessage = "RPC connection is unavailable";
        return nullptr;
    }
    std::shared_ptr<minireactor::TcpConnection> connection = session->tcpClient->connection();
    if (!connection) {
        errorCode = RpcErrorCode::NetworkError;
        errorMessage = "RPC connection is unavailable";
        return nullptr;
    }
    return connection;
}

RpcClient::~RpcClient() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
        acceptingConnection_ = false;
        connectingCount_ = 0;
        connected_ = false;
        reconnectEnabled_ = false;
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
            std::vector<std::shared_ptr<Session>> allSessions;
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                allSessions.reserve(sessions_.size());
                for (auto& item : sessions_) {
                    cancelReconnect(item.second);
                    allSessions.push_back(item.second);
                }
                sessions_.clear();
            }
            for (const auto& session : allSessions) {
                stopSession(session);
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

    if (discovery_) {
        const std::vector<Endpoint> endpoints = discovery_->discover(serviceName_);
        if (endpoints.empty()) {
            return false;
        }
        for (const Endpoint& endpoint : endpoints) {
            createSession(endpoint);
        }
    }

    std::vector<std::shared_ptr<Session>> toConnect;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        toConnect.reserve(sessions_.size());
        for (auto& item : sessions_) {
            if (!item.second->connected && !item.second->connecting && item.second->tcpClient) {
                cancelReconnect(item.second);
                item.second->connecting = true;
                toConnect.push_back(item.second);
            }
        }
    }

    bool startConnection = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (stopping_) {
            return false;
        }
        if (toConnect.empty()) {
            return connected_;
        }
        acceptingConnection_ = true;
        connectingCount_ += static_cast<int>(toConnect.size());
        startConnection = true;
    }
    if (startConnection) {
        for (const auto& session : toConnect) {
            session->tcpClient->connect();
        }
    }

    bool timedOut = false;
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (!stateCondition_.wait_for(lock, timeout, [this] { return connectingCount_ == 0; })) {
        connectingCount_ = 0;
        acceptingConnection_ = false;
        timedOut = true;
    }
    const bool result = connected_;
    lock.unlock();
    if (timedOut) {
        std::lock_guard<std::mutex> sessionsLock(sessionsMutex_);
        for (auto& item : sessions_) {
            if (item.second->connecting) {
                item.second->connecting = false;
                stopSession(item.second);
            }
        }
    }
    return result;
}

void RpcClient::disconnect() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        acceptingConnection_ = false;
        connectingCount_ = 0;
        connected_ = false;
        reconnectEnabled_ = false;
    }
    stateCondition_.notify_all();
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = false;
    }
    failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
    std::vector<std::shared_ptr<Session>> allSessions;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        allSessions.reserve(sessions_.size());
        for (auto& item : sessions_) {
            item.second->connected = false;
            item.second->connecting = false;
            cancelReconnect(item.second);
            allSessions.push_back(item.second);
        }
    }
    for (const auto& session : allSessions) {
        stopSession(session);
    }
    if (!loop_->isInLoopThread()) {
        std::promise<void> stopped;
        std::future<void> stoppedFuture = stopped.get_future();
        loop_->queueInLoop([&stopped] { stopped.set_value(); });
        stoppedFuture.get();
    }
}

bool RpcClient::connected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connected_;
}

std::future<RpcResponse> RpcClient::asyncCall(RpcRequest request,
                                              std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("RPC timeout must be greater than zero");
    }
    if (request.request_id == 0) {
        request.request_id = nextRequestId();
    }
    const std::uint64_t requestId = request.request_id;
    auto pending = std::make_shared<PendingCall>();
    std::future<RpcResponse> future = pending->promise.get_future();

    RpcErrorCode errorCode = RpcErrorCode::NetworkError;
    std::string errorMessage;
    std::shared_ptr<minireactor::TcpConnection> connection =
        selectConnection(request, errorCode, errorMessage);
    if (!connection) {
        completeCall(pending, errorResponse(requestId, errorCode, std::move(errorMessage)));
        return future;
    }

    std::string frame;
    try {
        frame = codec_.encodeRequest(request);
    } catch (const std::exception& error) {
        completeCall(pending, errorResponse(requestId, RpcErrorCode::ProtocolError, error.what()));
        return future;
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (!acceptingCalls_) {
            completeCall(pending, errorResponse(requestId, RpcErrorCode::NetworkError,
                                                "RPC connection is closing"));
            return future;
        }
        if (!pendingCalls_.emplace(requestId, pending).second) {
            completeCall(pending, errorResponse(requestId, RpcErrorCode::ProtocolError,
                                                "duplicate request id"));
            return future;
        }
        pending->timerId = loop_->runAfter(
            std::chrono::duration<double>(timeout).count(),
            [this, requestId] { onTimeout(requestId); });
    }

    connection->send(std::move(frame));
    return future;
}

RpcResponse RpcClient::call(RpcRequest request, std::chrono::milliseconds timeout) {
    if (loop_->isInLoopThread()) {
        throw std::logic_error("RpcClient::call cannot block its EventLoop thread");
    }
    if (maxRetries_ == 0) {
        return asyncCall(std::move(request), timeout).get();
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    RpcResponse lastResponse;
    for (std::size_t attempt = 0; attempt <= maxRetries_; ++attempt) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return lastResponse;
        }
        const auto attemptsLeft = static_cast<std::int64_t>(maxRetries_ - attempt + 1);
        const auto attemptTimeout =
            std::max(std::chrono::milliseconds(1), remaining / attemptsLeft);
        lastResponse = asyncCall(request, attemptTimeout).get();
        if (!isRetryable(lastResponse.error_code)) {
            return lastResponse;
        }
    }
    return lastResponse;
}

void RpcClient::setMaxRetries(std::size_t maxRetries) {
    maxRetries_ = maxRetries;
}

void RpcClient::setReconnectPolicy(std::chrono::milliseconds baseDelay,
                                   std::chrono::milliseconds maxDelay) {
    if (baseDelay <= std::chrono::milliseconds::zero() || maxDelay < baseDelay) {
        throw std::invalid_argument("invalid reconnect policy");
    }
    reconnectBaseDelay_ = std::chrono::duration<double>(baseDelay).count();
    reconnectMaxDelay_ = std::chrono::duration<double>(maxDelay).count();
}

void RpcClient::setReconnectEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        reconnectEnabled_ = enabled;
    }
    if (!enabled) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& item : sessions_) {
            cancelReconnect(item.second);
        }
    }
}

void RpcClient::scheduleReconnect(const std::shared_ptr<Session>& session) {
    if (!session) {
        return;
    }
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        enabled = !stopping_ && acceptingConnection_ && reconnectEnabled_;
    }
    if (!enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    if (session->connected || session->connecting || session->reconnectScheduled) {
        return;
    }
    session->reconnectScheduled = true;
    std::weak_ptr<Session> weakSession = session;
    session->reconnectTimerId = loop_->runAfter(
        reconnectDelay(reconnectBaseDelay_, reconnectMaxDelay_, session->reconnectAttempts),
        [this, weakSession] { tryReconnect(weakSession); });
}

void RpcClient::cancelReconnect(const std::shared_ptr<Session>& session) {
    if (!session) {
        return;
    }
    if (session->reconnectScheduled && session->reconnectTimerId != 0) {
        loop_->cancel(session->reconnectTimerId);
        session->reconnectTimerId = 0;
        session->reconnectScheduled = false;
    }
}

void RpcClient::tryReconnect(const std::weak_ptr<Session>& weakSession) {
    const std::shared_ptr<Session> session = weakSession.lock();
    if (!session) {
        return;
    }
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        enabled = !stopping_ && acceptingConnection_ && reconnectEnabled_;
    }
    if (!enabled) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        session->reconnectScheduled = false;
        session->reconnectTimerId = 0;
        return;
    }
    bool shouldConnect = false;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (session->connected || session->connecting || !session->reconnectScheduled) {
            session->reconnectScheduled = false;
            session->reconnectTimerId = 0;
            return;
        }
        session->reconnectScheduled = false;
        session->reconnectTimerId = 0;
        session->connecting = true;
        shouldConnect = true;
    }
    if (shouldConnect && session->tcpClient) {
        session->tcpClient->connect();
    }
}

double RpcClient::reconnectDelay(double baseDelay, double maxDelay,
                                 std::size_t attempts) {
    const double delay = baseDelay * std::pow(2.0, static_cast<double>(attempts));
    return std::min(delay, maxDelay);
}

bool RpcClient::isRetryable(int errorCode) {
    switch (static_cast<RpcErrorCode>(errorCode)) {
        case RpcErrorCode::NetworkError:
        case RpcErrorCode::Timeout:
            return true;
        default:
            return false;
    }
}

void RpcClient::onConnection(
    const std::string& key, const std::shared_ptr<minireactor::TcpConnection>& connection) {
    const bool isConnected = connection->state() == minireactor::TcpConnection::State::kConnected;
    bool rejectConnection = false;
    bool anyConnected = false;
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto found = sessions_.find(key);
        if (found != sessions_.end()) {
            session = found->second;
            session->connecting = false;
            session->connected = isConnected;
            if (isConnected) {
                session->reconnectAttempts = 0;
                session->reconnectScheduled = false;
                session->reconnectTimerId = 0;
            }
        }
        for (const auto& item : sessions_) {
            if (item.second->connected) {
                anyConnected = true;
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        rejectConnection = stopping_ || !acceptingConnection_;
        if (connectingCount_ > 0) {
            --connectingCount_;
        }
        connected_ = anyConnected && !stopping_;
        if (rejectConnection) {
            connected_ = false;
        }
    }
    if (!rejectConnection && isConnected) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        acceptingCalls_ = true;
    }
    if (!anyConnected || rejectConnection) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (!discovery_ || !anyConnected) {
            acceptingCalls_ = false;
        }
    }
    stateCondition_.notify_all();

    if (rejectConnection && isConnected) {
        loop_->queueInLoop([this, session] {
            if (session) {
                {
                    std::lock_guard<std::mutex> lock(sessionsMutex_);
                    session->connected = false;
                }
                stopSession(session);
            }
        });
        return;
    }

    if (!isConnected) {
        scheduleReconnect(session);
        if (!discovery_) {
            failAllPending(RpcErrorCode::NetworkError, "RPC connection closed");
        }
    }
}

void RpcClient::onConnectionError(const std::string& key, int error) {
    std::shared_ptr<Session> session;
    bool anyConnected = false;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto found = sessions_.find(key);
        if (found != sessions_.end()) {
            session = found->second;
            session->connecting = false;
            session->connected = false;
            ++session->reconnectAttempts;
        }
        for (const auto& item : sessions_) {
            if (item.second->connected) {
                anyConnected = true;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (connectingCount_ > 0) {
            --connectingCount_;
        }
        connected_ = anyConnected && !stopping_;
    }
    stateCondition_.notify_all();
    scheduleReconnect(session);
    if (!discovery_ || !anyConnected) {
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            acceptingCalls_ = false;
        }
        failAllPending(RpcErrorCode::NetworkError,
                       std::error_code(error, std::generic_category()).message());
    }
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
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& item : sessions_) {
                stopSession(item.second);
            }
            return;
        }
        completeResponse(std::move(response));
    }
}

void RpcClient::onTimeout(std::uint64_t requestId) {
    auto pending = takePending(requestId);
    if (pending) {
        completeCall(pending, errorResponse(requestId, RpcErrorCode::Timeout,
                                            "RPC call timed out"));
    }
}

std::shared_ptr<RpcClient::PendingCall> RpcClient::takePending(std::uint64_t requestId) {
    std::shared_ptr<PendingCall> pending;
    std::uint64_t timerId = 0;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const auto found = pendingCalls_.find(requestId);
        if (found == pendingCalls_.end()) {
            return nullptr;
        }
        pending = found->second;
        timerId = pending->timerId;
        pending->timerId = 0;
        pendingCalls_.erase(found);
    }
    if (timerId != 0) {
        loop_->cancel(timerId);
    }
    return pending;
}

void RpcClient::completeResponse(RpcResponse response) {
    auto pending = takePending(response.request_id);
    if (pending) {
        completeCall(pending, std::move(response));
    }
}

void RpcClient::completeCall(const std::shared_ptr<PendingCall>& pending,
                             RpcResponse response) {
    bool expected = false;
    if (!pending->completed.compare_exchange_strong(expected, true)) {
        return;
    }
    pending->promise.set_value(std::move(response));
}

void RpcClient::failAllPending(RpcErrorCode errorCode, const std::string& errorMessage) {
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
        if (item.second->timerId != 0) {
            loop_->cancel(item.second->timerId);
            item.second->timerId = 0;
        }
        completeCall(item.second, errorResponse(item.first, errorCode, errorMessage));
    }
}

void RpcClient::stopSession(const std::shared_ptr<Session>& session) {
    if (session && session->tcpClient) {
        session->tcpClient->stop();
    }
}

std::uint64_t RpcClient::nextRequestId() {
    std::uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    if (requestId == 0) {
        requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    }
    return requestId;
}

RpcResponse RpcClient::errorResponse(std::uint64_t requestId, RpcErrorCode errorCode,
                                     std::string errorMessage) {
    RpcResponse response;
    response.request_id = requestId;
    response.error_code = static_cast<int>(errorCode);
    response.error_message = std::move(errorMessage);
    return response;
}

}  // namespace minirpc
