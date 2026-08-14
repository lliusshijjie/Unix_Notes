#include "net/TcpClient.h"

#include "net/Connector.h"
#include "net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace minireactor {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddress, std::string name)
    : loop_(loop),
      serverAddress_(serverAddress),
      name_(std::move(name)),
      connector_(std::make_shared<Connector>(loop, serverAddress_)) {
    if (loop_ == nullptr) {
        throw std::invalid_argument("TcpClient requires an EventLoop");
    }
    loop_->assertInLoopThread();
    connector_->setNewConnectionCallback([this](int socketFd) { newConnection(socketFd); });
    connector_->setErrorCallback([this](int error) {
        if (connectionErrorCallback_) {
            connectionErrorCallback_(error);
        }
    });
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();
    connector_->setNewConnectionCallback({});
    connector_->setErrorCallback({});
    connector_->stop();

    std::shared_ptr<TcpConnection> activeConnection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeConnection = std::move(connection_);
    }
    if (activeConnection) {
        activeConnection->setConnectionCallback({});
        activeConnection->setMessageCallback({});
        activeConnection->setCloseCallback({});
        activeConnection->forceClose();
        activeConnection->connectDestroyed();
    }
}

void TcpClient::setConnectionCallback(TcpConnection::ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

void TcpClient::setMessageCallback(TcpConnection::MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void TcpClient::setConnectionErrorCallback(ConnectionErrorCallback callback) {
    connectionErrorCallback_ = std::move(callback);
}

void TcpClient::connect() {
    connector_->start();
}

void TcpClient::disconnect() {
    std::shared_ptr<TcpConnection> activeConnection = connection();
    if (activeConnection) {
        activeConnection->shutdown();
    }
}

void TcpClient::stop() {
    connector_->stop();
    std::shared_ptr<TcpConnection> activeConnection = connection();
    if (activeConnection) {
        activeConnection->forceClose();
    }
}

std::shared_ptr<TcpConnection> TcpClient::connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

void TcpClient::newConnection(int socketFd) {
    loop_->assertInLoopThread();
    const std::string connectionName =
        name_ + "-connection-" + std::to_string(nextConnectionId_++);
    auto activeConnection = std::make_shared<TcpConnection>(loop_, connectionName, socketFd);
    activeConnection->setConnectionCallback(connectionCallback_);
    activeConnection->setMessageCallback(messageCallback_);
    activeConnection->setCloseCallback(
        [this](const std::shared_ptr<TcpConnection>& item) { removeConnection(item); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = activeConnection;
    }
    activeConnection->connectEstablished();
}

void TcpClient::removeConnection(const std::shared_ptr<TcpConnection>& activeConnection) {
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ == activeConnection) {
            connection_.reset();
        }
    }
    connector_->stop();
    activeConnection->connectDestroyed();
}

}  // namespace minireactor
