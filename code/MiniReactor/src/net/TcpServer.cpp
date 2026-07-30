#include "net/TcpServer.h"

#include "net/EventLoop.h"

#include <utility>

namespace minireactor {

TcpServer::TcpServer(EventLoop* loop, std::uint16_t port, const char* bindIp)
    : loop_(loop), acceptor_(loop, port, bindIp) {
    acceptor_.setNewConnectionCallback(
        [this](int socketFd, const sockaddr_in& peerAddress) { newConnection(socketFd, peerAddress); });
}

void TcpServer::setConnectionCallback(TcpConnection::ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

void TcpServer::setMessageCallback(TcpConnection::MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void TcpServer::start() { acceptor_.listen(); }

void TcpServer::newConnection(int socketFd, const sockaddr_in&) {
    const std::string connectionName = "connection-" + std::to_string(nextConnectionId_++);
    auto connection = std::make_shared<TcpConnection>(loop_, connectionName, socketFd);
    connection->setConnectionCallback(connectionCallback_);
    connection->setMessageCallback(messageCallback_);
    connection->setCloseCallback([this](const std::shared_ptr<TcpConnection>& item) {
        removeConnection(item);
    });
    connections_.emplace(connectionName, connection);
    connection->connectEstablished();
}

void TcpServer::removeConnection(const std::shared_ptr<TcpConnection>& connection) {
    const auto found = connections_.find(connection->name());
    if (found == connections_.end()) {
        return;
    }
    connection->connectDestroyed();
    connections_.erase(found);
}

}  // namespace minireactor
